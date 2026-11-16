// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Shared mod management application service: command grammar, confirmation,
// dry-run, audit records, install/uninstall through the content store, trust
// administration and the hot lifecycle port.

#include <refrain/app/mod_management.hpp>

#include <refrain/core/io.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/data/path.hpp>
#include <refrain/data/signature.hpp>
#include <refrain/mod/install_policy.hpp>
#include <refrain/mod/rfmod.hpp>
#include <refrain/platform/process.hpp>

#include <boost/ut.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace {

using namespace boost::ut;

namespace app = refrain::app;
namespace data = refrain::data;
namespace io = refrain::core::io;
namespace mod = refrain::mod;
namespace fs = std::filesystem;

constexpr std::string_view seed_a{
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"};
constexpr std::string_view key_a{"test-key-a"};

[[nodiscard]] auto make_workspace_path(std::string_view name) -> fs::path {
    static int counter{0};
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    const auto path{fs::temp_directory_path() /
                    fmt::format("refrain_mod_management_test_{}_{}_{}_{}",
                                stamp, counter++,
                                refrain::platform::process_id(), name)};
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

[[nodiscard]] auto source_manifest(std::string_view id,
                                   std::string_view version) -> std::string {
    return fmt::format("[mod]\nid = \"{}\"\nversion = \"{}\"\nabi = \"1\"\n"
                       "side = \"server\"\n\n[meta]\ndisplay_name = "
                       "\"Mod management test\"\n",
                       id, version);
}

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

[[nodiscard]] auto pack(const temp_workspace &work, std::string_view directory,
                        std::string_view output,
                        const mod::rfmod_write_options &options = {})
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

[[nodiscard]] auto unsigned_policy() -> mod::install_policy_config {
    mod::install_policy_config policy{};
    policy.require_signature = false;
    return policy;
}

/// Capturing audit sink plus the context every test fills in as needed.
struct fixture {
    temp_workspace work{"mod-management"};
    std::vector<app::mod_audit_record> audit{};
    app::mod_management_context context{};

    explicit fixture(std::string_view name) : work{name} {
        context.policy = unsigned_policy();
        context.audit = [this](const app::mod_audit_record &record) {
            audit.push_back(record);
        };
    }

    [[nodiscard]] auto service() -> app::mod_management {
        return app::mod_management{context};
    }

    [[nodiscard]] auto store_path() const -> std::string {
        return work.path("store").string();
    }

    [[nodiscard]] auto console() const -> app::mod_management_actor {
        return app::mod_management_actor{
            .kind = app::mod_management_actor_kind::console,
            .name = "@console",
            .uuid = {},
        };
    }

    [[nodiscard]] auto player() const -> app::mod_management_actor {
        return app::mod_management_actor{
            .kind = app::mod_management_actor_kind::player,
            .name = "Alice",
            .uuid = "11111111-1111-1111-1111-111111111111",
        };
    }

    [[nodiscard]] auto receipt_count() const -> std::size_t {
        auto paths{mod::list_receipt_paths(store_path())};
        if (!paths) {
            return 0;
        }
        return paths->size();
    }
};

[[nodiscard]] auto run(app::mod_management &service,
                       std::initializer_list<std::string_view> arguments,
                       const app::mod_management_actor &actor)
    -> app::mod_management_report {
    return service.execute(
        std::span<const std::string_view>{arguments.begin(), arguments.size()},
        actor);
}

[[nodiscard]] auto contains(std::span<const std::string> lines,
                            std::string_view needle) -> bool {
    for (const auto &line : lines) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] auto signed_keypair() -> data::ed25519_keypair {
    auto seed{data::parse_seed_hex(seed_a)};
    expect(static_cast<bool>(seed));
    if (!seed) {
        return {};
    }
    auto pair{data::keypair_from_seed(*seed)};
    expect(static_cast<bool>(pair));
    return pair ? *pair : data::ed25519_keypair{};
}

} // namespace

const suite mod_management_service_tests = [] {
    "mod management/list reports the running composition"_test = [] {
        fixture test{"list"};
        test.context.loaded.push_back(app::mod_management_mod{
            .id = "refrain.world.vanilla",
            .version = "1.0.0",
            .loader = "builtin",
            .has_code = true,
            .source = "builtin",
            .services = {"refrain:world"},
        });
        auto service{test.service()};
        const auto report{run(service, {"list"}, test.console())};
        expect(report.ok) << report.error;
        expect(report.lines.size() == 2U) << report.lines.size();
        expect(contains(report.lines, "refrain.world.vanilla 1.0.0"));
        expect(contains(report.lines, "refrain:world"));
        expect(test.audit.size() == 1U);
        expect(test.audit.front().action == "list");
        expect(test.audit.front().result == "ok");
        expect(test.audit.front().actor_kind == "console");
    };

    "mod management/install refuses without confirmation and audits it"_test =
        [] {
            fixture test{"install-refused"};
            expect(
                materialise(test.work, "mod", "refrain.test.manager", "1.0.0"));
            auto archive{pack(test.work, "mod", "out/mod.rfmod")};
            expect(archive.has_value());
            if (!archive) {
                return;
            }
            test.context.store = test.store_path();
            auto service{test.service()};
            const auto report{
                run(service, {"install", *archive}, test.console())};
            expect(!report.ok);
            expect(report.outcome == "refused");
            expect(report.error.find("confirm") != std::string::npos)
                << report.error;
            expect(test.receipt_count() == 0U);
            expect(test.audit.size() == 1U);
            expect(test.audit.front().action == "install");
            expect(test.audit.front().result == "refused");
            expect(!test.audit.front().detail.empty());
        };

    "mod management/install with confirm writes a receipt and audits it"_test =
        [] {
            fixture test{"install-confirm"};
            expect(
                materialise(test.work, "mod", "refrain.test.manager", "1.0.0"));
            auto archive{pack(test.work, "mod", "out/mod.rfmod")};
            expect(archive.has_value());
            if (!archive) {
                return;
            }
            test.context.store = test.store_path();
            auto service{test.service()};
            const auto report{
                run(service, {"install", *archive, "confirm"}, test.console())};
            expect(report.ok) << report.error;
            // The store change is durable; with hot install off it lands on
            // the next start.
            expect(report.outcome == "restart-required");
            expect(test.receipt_count() == 1U);
            expect(test.audit.size() == 1U);
            const auto &record{test.audit.front()};
            expect(record.action == "install");
            expect(record.id == "refrain.test.manager");
            expect(record.version == "1.0.0");
            expect(record.digest.size() == 64U) << record.digest;
            expect(record.result == "restart-required");
            expect(contains(report.lines, "installed refrain.test.manager"));
        };

    "mod management/install --dry-run writes no store state"_test = [] {
        fixture test{"install-dry-run"};
        expect(materialise(test.work, "mod", "refrain.test.manager", "1.0.0"));
        auto archive{pack(test.work, "mod", "out/mod.rfmod")};
        expect(archive.has_value());
        if (!archive) {
            return;
        }
        test.context.store = test.store_path();
        auto service{test.service()};
        const auto report{
            run(service, {"install", *archive, "--dry-run"}, test.console())};
        expect(report.ok) << report.error;
        expect(report.outcome == "dry-run");
        expect(test.receipt_count() == 0U);
        expect(contains(report.lines, "dry run"));
        expect(test.audit.size() == 1U);
        expect(test.audit.front().result == "dry-run");
        expect(test.audit.front().id == "refrain.test.manager");
    };

    "mod management/install rejects a missing or wrong-suffix path"_test = [] {
        fixture test{"install-path"};
        test.context.store = test.store_path();
        auto service{test.service()};
        const auto missing{run(service,
                               {"install", "does-not-exist.rfmod", "confirm"},
                               test.console())};
        expect(!missing.ok);
        expect(missing.error.find(".rfmod") != std::string::npos)
            << missing.error;
        expect(missing.outcome == "failed");

        expect(test.work.write("not-a-package.txt", "hello"));
        const auto suffix{
            run(service,
                {"install", test.work.path("not-a-package.txt").string(),
                 "confirm"},
                test.console())};
        expect(!suffix.ok);
        expect(suffix.error.find(".rfmod") != std::string::npos)
            << suffix.error;
    };

    "mod management/uninstall requires confirmation"_test = [] {
        fixture test{"uninstall-refused"};
        expect(materialise(test.work, "mod", "refrain.test.manager", "1.0.0"));
        auto archive{pack(test.work, "mod", "out/mod.rfmod")};
        expect(archive.has_value());
        if (!archive) {
            return;
        }
        test.context.store = test.store_path();
        auto service{test.service()};
        expect(
            run(service, {"install", *archive, "confirm"}, test.console()).ok);
        const auto report{run(service, {"uninstall", "refrain.test.manager"},
                              test.console())};
        expect(!report.ok);
        expect(report.outcome == "refused");
        expect(report.error.find("confirm") != std::string::npos);
        expect(test.receipt_count() == 1U);
    };

    "mod management/uninstall removes the receipt and audits it"_test = [] {
        fixture test{"uninstall-confirm"};
        expect(materialise(test.work, "mod", "refrain.test.manager", "1.0.0"));
        auto archive{pack(test.work, "mod", "out/mod.rfmod")};
        expect(archive.has_value());
        if (!archive) {
            return;
        }
        test.context.store = test.store_path();
        auto service{test.service()};
        expect(
            run(service, {"install", *archive, "confirm"}, test.console()).ok);
        test.audit.clear();
        const auto report{
            run(service, {"uninstall", "refrain.test.manager@1.0.0", "confirm"},
                test.console())};
        expect(report.ok) << report.error;
        expect(test.receipt_count() == 0U);
        expect(
            contains(report.lines, "uninstalled refrain.test.manager 1.0.0"));
        expect(test.audit.size() == 1U);
        expect(test.audit.front().action == "uninstall");
        expect(test.audit.front().id == "refrain.test.manager");
        expect(test.audit.front().version == "1.0.0");
    };

    "mod management/installed lists receipts of the target store"_test = [] {
        fixture test{"installed"};
        expect(materialise(test.work, "mod", "refrain.test.manager", "1.0.0"));
        auto archive{pack(test.work, "mod", "out/mod.rfmod")};
        expect(archive.has_value());
        if (!archive) {
            return;
        }
        test.context.store = test.store_path();
        auto service{test.service()};
        const auto empty{run(service, {"installed"}, test.console())};
        expect(empty.ok);
        expect(contains(empty.lines, "no mods are installed"));
        expect(
            run(service, {"install", *archive, "confirm"}, test.console()).ok);
        const auto listed{run(service, {"installed"}, test.console())};
        expect(listed.ok);
        expect(contains(listed.lines, "refrain.test.manager 1.0.0"));
    };

    "mod management/installed without a content store reports the gap"_test =
        [] {
            fixture test{"no-store"};
            auto service{test.service()};
            const auto report{run(service, {"installed"}, test.console())};
            expect(!report.ok);
            expect(report.error.find("--content-store") != std::string::npos)
                << report.error;
            expect(test.audit.size() == 1U);
            expect(test.audit.front().result == "failed");
        };

    "mod management/verify reports a signed package and its trust"_test = [] {
        fixture test{"verify"};
        expect(materialise(test.work, "mod", "refrain.test.manager", "1.0.0"));
        const auto keypair{signed_keypair()};
        auto archive{
            pack(test.work, "mod", "out/mod.rfmod",
                 mod::rfmod_write_options{.signing = mod::rfmod_signing_options{
                                              .secret_key = keypair.secret_key,
                                              .key_id = std::string{key_a}}})};
        expect(archive.has_value());
        if (!archive) {
            return;
        }
        test.context.policy.require_signature = true;
        test.context.policy.trusted_keys.push_back(mod::rfmod_trusted_key{
            .key_id = std::string{key_a}, .public_key = keypair.public_key});
        auto service{test.service()};
        const auto report{run(service, {"verify", *archive}, test.player())};
        expect(report.ok) << report.error;
        expect(report.lines.size() == 4U);
        expect(contains(report.lines, "trusted"));
        expect(test.audit.size() == 1U);
        expect(test.audit.front().key_id == key_a);
        expect(test.audit.front().digest.size() == 64U);
        expect(test.audit.front().actor_kind == "player");
        expect(test.audit.front().actor_uuid ==
               "11111111-1111-1111-1111-111111111111");
    };

    "mod management/reload is refused when hot reload is disabled"_test = [] {
        fixture test{"reload-gate"};
        test.context.loaded.push_back(app::mod_management_mod{
            .id = "refrain.test.manager",
            .version = "1.0.0",
            .loader = "native",
            .has_code = true,
            .source = "installed",
        });
        test.context.hot_reload = false;
        auto service{test.service()};
        const auto report{
            run(service, {"reload", "refrain.test.manager"}, test.console())};
        expect(!report.ok);
        expect(report.outcome == "refused");
        expect(report.error.find("hot reload is disabled") != std::string::npos)
            << report.error;
        expect(test.audit.size() == 1U);
        expect(test.audit.front().action == "reload");
        expect(test.audit.front().result == "refused");
    };

    "mod management/reload is unsupported without a lifecycle port"_test = [] {
        fixture test{"reload-unsupported"};
        test.context.loaded.push_back(app::mod_management_mod{
            .id = "refrain.test.manager",
            .version = "1.0.0",
            .loader = "native",
            .has_code = true,
            .source = "installed",
        });
        test.context.hot_reload = true;
        test.context.lifecycle.unavailable_reason =
            "the hot lifecycle registry is not available in this build";
        auto service{test.service()};
        const auto report{
            run(service, {"reload", "refrain.test.manager"}, test.console())};
        expect(!report.ok);
        expect(report.outcome == "unsupported");
        expect(report.error.find("hot lifecycle registry") != std::string::npos)
            << report.error;
        expect(test.audit.size() == 1U);
        expect(test.audit.front().result == "unsupported");
    };

    "mod management/reload hands the request to the lifecycle port"_test = [] {
        fixture test{"reload-port"};
        test.context.loaded.push_back(app::mod_management_mod{
            .id = "refrain.test.manager",
            .version = "1.0.0",
            .loader = "native",
            .has_code = true,
            .source = "installed",
        });
        test.context.hot_reload = true;
        std::vector<app::mod_lifecycle_request> requests{};
        test.context.lifecycle.apply =
            [&requests](const app::mod_lifecycle_request &request)
            -> refrain::result<app::mod_lifecycle_outcome> {
            requests.push_back(request);
            return app::mod_lifecycle_outcome{
                .lines = {
                    "reloaded refrain.test.manager 1.0.0 as generation 2"}};
        };
        auto service{test.service()};
        const auto report{
            run(service, {"reload", "refrain.test.manager"}, test.console())};
        expect(report.ok) << report.error;
        expect(report.outcome == "ok");
        expect(requests.size() == 1U);
        expect(requests.front().action == app::mod_lifecycle_action::reload);
        expect(requests.front().id == "refrain.test.manager");
        expect(requests.front().version == "1.0.0");
        expect(contains(report.lines, "generation 2"));
    };

    "mod management/install with hot install disabled requires a restart"_test =
        [] {
            fixture test{"install-restart"};
            expect(
                materialise(test.work, "mod", "refrain.test.manager", "1.0.0"));
            auto archive{pack(test.work, "mod", "out/mod.rfmod")};
            expect(archive.has_value());
            if (!archive) {
                return;
            }
            test.context.store = test.store_path();
            test.context.hot_install = false;
            auto service{test.service()};
            const auto report{
                run(service, {"install", *archive, "confirm"}, test.console())};
            expect(report.ok) << report.error;
            expect(report.outcome == "restart-required");
            expect(contains(report.lines, "restart required"));
            expect(test.receipt_count() == 1U);
            expect(test.audit.front().result == "restart-required");
        };

    "mod management/install with hot install but no lifecycle port is "
    "unsupported"_test = [] {
        fixture test{"install-unsupported"};
        expect(materialise(test.work, "mod", "refrain.test.manager", "1.0.0"));
        auto archive{pack(test.work, "mod", "out/mod.rfmod")};
        expect(archive.has_value());
        if (!archive) {
            return;
        }
        test.context.store = test.store_path();
        test.context.hot_install = true;
        test.context.lifecycle.unavailable_reason =
            "the hot lifecycle registry is not available in this build";
        auto service{test.service()};
        const auto report{
            run(service, {"install", *archive, "confirm"}, test.console())};
        expect(!report.ok);
        expect(report.outcome == "unsupported");
        expect(report.error.find("hot lifecycle registry") != std::string::npos)
            << report.error;
        // The store change is durable even though activation failed.
        expect(test.receipt_count() == 1U);
    };

    "mod management/trust lists the policy and the signer pins"_test = [] {
        fixture test{"trust-list"};
        test.context.policy.require_signature = true;
        test.context.policy.signature_policy =
            mod::rfmod_signature_policy::strict;
        const auto keypair{signed_keypair()};
        test.context.policy.trusted_keys.push_back(mod::rfmod_trusted_key{
            .key_id = std::string{key_a}, .public_key = keypair.public_key});
        expect(materialise(test.work, "mod", "refrain.test.manager", "1.0.0"));
        auto archive{
            pack(test.work, "mod", "out/mod.rfmod",
                 mod::rfmod_write_options{.signing = mod::rfmod_signing_options{
                                              .secret_key = keypair.secret_key,
                                              .key_id = std::string{key_a}}})};
        expect(archive.has_value());
        if (!archive) {
            return;
        }
        test.context.store = test.store_path();
        auto service{test.service()};
        expect(
            run(service, {"install", *archive, "confirm"}, test.console()).ok);
        test.audit.clear();
        const auto report{run(service, {"trust"}, test.console())};
        expect(report.ok) << report.error;
        expect(contains(report.lines, "trust anchor 'test-key-a'"));
        expect(contains(report.lines, "signer pin for refrain.test.manager"));
        expect(test.audit.size() == 1U);
        expect(test.audit.front().action == "trust-list");
    };

    "mod management/trust pin requires confirmation and writes the pin"_test =
        [] {
            fixture test{"trust-pin"};
            const auto keypair{signed_keypair()};
            test.context.policy.require_signature = true;
            test.context.policy.trusted_keys.push_back(
                mod::rfmod_trusted_key{.key_id = std::string{key_a},
                                       .public_key = keypair.public_key});
            expect(
                materialise(test.work, "mod", "refrain.test.manager", "1.0.0"));
            auto archive{pack(
                test.work, "mod", "out/mod.rfmod",
                mod::rfmod_write_options{.signing = mod::rfmod_signing_options{
                                             .secret_key = keypair.secret_key,
                                             .key_id = std::string{key_a}}})};
            expect(archive.has_value());
            if (!archive) {
                return;
            }
            test.context.store = test.store_path();
            auto service{test.service()};
            expect(
                run(service, {"install", *archive, "confirm"}, test.console())
                    .ok);
            expect(
                mod::clear_signer_pin(test.store_path(), "refrain.test.manager")
                    .has_value());
            test.audit.clear();
            const auto refused{run(service,
                                   {"trust", "pin", "refrain.test.manager"},
                                   test.console())};
            expect(!refused.ok);
            expect(refused.outcome == "refused");
            const auto pinned{run(
                service, {"trust", "pin", "refrain.test.manager", "confirm"},
                test.console())};
            expect(pinned.ok) << pinned.error;
            auto pin{mod::read_signer_pin(test.store_path(),
                                          "refrain.test.manager")};
            expect(static_cast<bool>(pin));
            if (pin && pin->has_value()) {
                expect((*pin)->key_id == key_a);
                expect((*pin)->source == "admin");
            }
            expect(test.audit.size() == 2U);
            expect(test.audit.back().result == "ok");
            expect(test.audit.back().key_id == key_a);
        };

    "mod management/unknown command and bad usage fail cleanly"_test = [] {
        fixture test{"usage"};
        auto service{test.service()};
        const auto unknown{run(service, {"frobnicate"}, test.console())};
        expect(!unknown.ok);
        expect(unknown.error.find("available mod commands") !=
               std::string::npos);
        const auto bad_option{
            run(service, {"list", "--force"}, test.console())};
        expect(!bad_option.ok);
        expect(bad_option.error.find("unknown option") != std::string::npos);
        const auto no_store{
            run(service, {"reload", "missing.mod"}, test.console())};
        expect(!no_store.ok);
        expect(no_store.error.find("running composition") != std::string::npos);
        // Every line, including a usage failure, is audited.
        expect(test.audit.size() == 3U);
    };

    "mod management/audit line names actor, action, package and result"_test =
        [] {
            app::mod_audit_record record{};
            record.actor_kind = "player";
            record.actor_name = "Alice";
            record.actor_uuid = "11111111-1111-1111-1111-111111111111";
            record.action = "install";
            record.id = "refrain.test.manager";
            record.version = "1.0.0";
            record.digest = std::string(64, 'a');
            record.key_id = "test-key-a";
            record.policy = "strict";
            record.result = "ok";
            record.duration_ms = 3;
            const auto line{app::format_mod_audit_record(record)};
            expect(line.find("player 11111111-1111-1111-1111-111111111111") !=
                   std::string::npos)
                << line;
            expect(line.find("ran install on refrain.test.manager 1.0.0") !=
                   std::string::npos)
                << line;
            expect(line.find("signed by key 'test-key-a'") != std::string::npos)
                << line;
            expect(line.find("the result was ok") != std::string::npos) << line;
        };
};
