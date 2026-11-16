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
        fixture env{"list"};
        env.context.loaded.push_back(app::mod_management_mod{
            .id = "refrain.world.vanilla",
            .version = "1.0.0",
            .loader = "builtin",
            .has_code = true,
            .source = "builtin",
            .services = {"refrain:world"},
        });
        auto service{env.service()};
        const auto report{run(service, {"list"}, env.console())};
        expect(report.ok) << report.error;
        expect(report.lines.size() == 2U) << report.lines.size();
        expect(contains(report.lines, "refrain.world.vanilla 1.0.0"));
        expect(contains(report.lines, "refrain:world"));
        expect(env.audit.size() == 1U);
        expect(env.audit.front().action == "list");
        expect(env.audit.front().result == "ok");
        expect(env.audit.front().actor_kind == "console");
    };

    "mod management/install refuses without confirmation and audits it"_test =
        [] {
            fixture env{"install-refused"};
            expect(
                materialise(env.work, "mod", "refrain.test.manager", "1.0.0"));
            auto archive{pack(env.work, "mod", "out/mod.rfmod")};
            expect(archive.has_value());
            if (!archive) {
                return;
            }
            env.context.store = env.store_path();
            auto service{env.service()};
            const auto report{
                run(service, {"install", *archive}, env.console())};
            expect(!report.ok);
            expect(report.outcome == "refused");
            expect(report.error.find("confirm") != std::string::npos)
                << report.error;
            expect(env.receipt_count() == 0U);
            expect(env.audit.size() == 1U);
            expect(env.audit.front().action == "install");
            expect(env.audit.front().result == "refused");
            expect(!env.audit.front().detail.empty());
        };

    "mod management/install with confirm writes a receipt and audits it"_test =
        [] {
            fixture env{"install-confirm"};
            expect(
                materialise(env.work, "mod", "refrain.test.manager", "1.0.0"));
            auto archive{pack(env.work, "mod", "out/mod.rfmod")};
            expect(archive.has_value());
            if (!archive) {
                return;
            }
            env.context.store = env.store_path();
            auto service{env.service()};
            const auto report{
                run(service, {"install", *archive, "confirm"}, env.console())};
            expect(report.ok) << report.error;
            // The store change is durable; with hot install off it lands on
            // the next start.
            expect(report.outcome == "restart-required");
            expect(env.receipt_count() == 1U);
            expect(env.audit.size() == 1U);
            const auto &record{env.audit.front()};
            expect(record.action == "install");
            expect(record.id == "refrain.test.manager");
            expect(record.version == "1.0.0");
            expect(record.digest.size() == 64U) << record.digest;
            expect(record.result == "restart-required");
            expect(contains(report.lines, "installed refrain.test.manager"));
        };

    "mod management/install --dry-run writes no store state"_test = [] {
        fixture env{"install-dry-run"};
        expect(materialise(env.work, "mod", "refrain.test.manager", "1.0.0"));
        auto archive{pack(env.work, "mod", "out/mod.rfmod")};
        expect(archive.has_value());
        if (!archive) {
            return;
        }
        env.context.store = env.store_path();
        auto service{env.service()};
        const auto report{
            run(service, {"install", *archive, "--dry-run"}, env.console())};
        expect(report.ok) << report.error;
        expect(report.outcome == "dry-run");
        expect(env.receipt_count() == 0U);
        expect(contains(report.lines, "dry run"));
        expect(env.audit.size() == 1U);
        expect(env.audit.front().result == "dry-run");
        expect(env.audit.front().id == "refrain.test.manager");
    };

    "mod management/install rejects a missing or wrong-suffix path"_test = [] {
        fixture env{"install-path"};
        env.context.store = env.store_path();
        auto service{env.service()};
        const auto missing{run(service,
                               {"install", "does-not-exist.rfmod", "confirm"},
                               env.console())};
        expect(!missing.ok);
        expect(missing.error.find(".rfmod") != std::string::npos)
            << missing.error;
        expect(missing.outcome == "failed");

        expect(env.work.write("not-a-package.txt", "hello"));
        const auto suffix{run(
            service,
            {"install", env.work.path("not-a-package.txt").string(), "confirm"},
            env.console())};
        expect(!suffix.ok);
        expect(suffix.error.find(".rfmod") != std::string::npos)
            << suffix.error;
    };

    "mod management/uninstall requires confirmation"_test = [] {
        fixture env{"uninstall-refused"};
        expect(materialise(env.work, "mod", "refrain.test.manager", "1.0.0"));
        auto archive{pack(env.work, "mod", "out/mod.rfmod")};
        expect(archive.has_value());
        if (!archive) {
            return;
        }
        env.context.store = env.store_path();
        auto service{env.service()};
        expect(
            run(service, {"install", *archive, "confirm"}, env.console()).ok);
        const auto report{
            run(service, {"uninstall", "refrain.test.manager"}, env.console())};
        expect(!report.ok);
        expect(report.outcome == "refused");
        expect(report.error.find("confirm") != std::string::npos);
        expect(env.receipt_count() == 1U);
    };

    "mod management/uninstall removes the receipt and audits it"_test = [] {
        fixture env{"uninstall-confirm"};
        expect(materialise(env.work, "mod", "refrain.test.manager", "1.0.0"));
        auto archive{pack(env.work, "mod", "out/mod.rfmod")};
        expect(archive.has_value());
        if (!archive) {
            return;
        }
        env.context.store = env.store_path();
        auto service{env.service()};
        expect(
            run(service, {"install", *archive, "confirm"}, env.console()).ok);
        env.audit.clear();
        const auto report{
            run(service, {"uninstall", "refrain.test.manager@1.0.0", "confirm"},
                env.console())};
        expect(report.ok) << report.error;
        expect(env.receipt_count() == 0U);
        expect(
            contains(report.lines, "uninstalled refrain.test.manager 1.0.0"));
        expect(env.audit.size() == 1U);
        expect(env.audit.front().action == "uninstall");
        expect(env.audit.front().id == "refrain.test.manager");
        expect(env.audit.front().version == "1.0.0");
    };

    "mod management/installed lists receipts of the target store"_test = [] {
        fixture env{"installed"};
        expect(materialise(env.work, "mod", "refrain.test.manager", "1.0.0"));
        auto archive{pack(env.work, "mod", "out/mod.rfmod")};
        expect(archive.has_value());
        if (!archive) {
            return;
        }
        env.context.store = env.store_path();
        auto service{env.service()};
        const auto empty{run(service, {"installed"}, env.console())};
        expect(empty.ok);
        expect(contains(empty.lines, "no mods are installed"));
        expect(
            run(service, {"install", *archive, "confirm"}, env.console()).ok);
        const auto listed{run(service, {"installed"}, env.console())};
        expect(listed.ok);
        expect(contains(listed.lines, "refrain.test.manager 1.0.0"));
    };

    "mod management/installed without a content store reports the gap"_test =
        [] {
            fixture env{"no-store"};
            auto service{env.service()};
            const auto report{run(service, {"installed"}, env.console())};
            expect(!report.ok);
            expect(report.error.find("--content-store") != std::string::npos)
                << report.error;
            expect(env.audit.size() == 1U);
            expect(env.audit.front().result == "failed");
        };

    "mod management/verify reports a signed package and its trust"_test = [] {
        fixture env{"verify"};
        expect(materialise(env.work, "mod", "refrain.test.manager", "1.0.0"));
        const auto keypair{signed_keypair()};
        auto archive{
            pack(env.work, "mod", "out/mod.rfmod",
                 mod::rfmod_write_options{.signing = mod::rfmod_signing_options{
                                              .secret_key = keypair.secret_key,
                                              .key_id = std::string{key_a}}})};
        expect(archive.has_value());
        if (!archive) {
            return;
        }
        env.context.policy.require_signature = true;
        env.context.policy.trusted_keys.push_back(mod::rfmod_trusted_key{
            .key_id = std::string{key_a}, .public_key = keypair.public_key});
        auto service{env.service()};
        const auto report{run(service, {"verify", *archive}, env.player())};
        expect(report.ok) << report.error;
        expect(report.lines.size() == 4U);
        expect(contains(report.lines, "trusted"));
        expect(env.audit.size() == 1U);
        expect(env.audit.front().key_id == key_a);
        expect(env.audit.front().digest.size() == 64U);
        expect(env.audit.front().actor_kind == "player");
        expect(env.audit.front().actor_uuid ==
               "11111111-1111-1111-1111-111111111111");
    };

    "mod management/reload is refused when hot reload is disabled"_test = [] {
        fixture env{"reload-gate"};
        env.context.loaded.push_back(app::mod_management_mod{
            .id = "refrain.test.manager",
            .version = "1.0.0",
            .loader = "native",
            .has_code = true,
            .source = "installed",
        });
        env.context.hot_reload = false;
        auto service{env.service()};
        const auto report{
            run(service, {"reload", "refrain.test.manager"}, env.console())};
        expect(!report.ok);
        expect(report.outcome == "refused");
        expect(report.error.find("hot reload is disabled") != std::string::npos)
            << report.error;
        expect(env.audit.size() == 1U);
        expect(env.audit.front().action == "reload");
        expect(env.audit.front().result == "refused");
    };

    "mod management/reload is unsupported without a lifecycle port"_test = [] {
        fixture env{"reload-unsupported"};
        env.context.loaded.push_back(app::mod_management_mod{
            .id = "refrain.test.manager",
            .version = "1.0.0",
            .loader = "native",
            .has_code = true,
            .source = "installed",
        });
        env.context.hot_reload = true;
        env.context.lifecycle.unavailable_reason =
            "the hot lifecycle registry is not available in this build";
        auto service{env.service()};
        const auto report{
            run(service, {"reload", "refrain.test.manager"}, env.console())};
        expect(!report.ok);
        expect(report.outcome == "unsupported");
        expect(report.error.find("hot lifecycle registry") != std::string::npos)
            << report.error;
        expect(env.audit.size() == 1U);
        expect(env.audit.front().result == "unsupported");
    };

    "mod management/reload hands the request to the lifecycle port"_test = [] {
        fixture env{"reload-port"};
        env.context.loaded.push_back(app::mod_management_mod{
            .id = "refrain.test.manager",
            .version = "1.0.0",
            .loader = "native",
            .has_code = true,
            .source = "installed",
        });
        env.context.hot_reload = true;
        std::vector<app::mod_lifecycle_request> requests{};
        env.context.lifecycle.apply =
            [&requests](const app::mod_lifecycle_request &request)
            -> refrain::result<app::mod_lifecycle_outcome> {
            requests.push_back(request);
            return app::mod_lifecycle_outcome{
                .lines = {
                    "reloaded refrain.test.manager 1.0.0 as generation 2"}};
        };
        auto service{env.service()};
        const auto report{
            run(service, {"reload", "refrain.test.manager"}, env.console())};
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
            fixture env{"install-restart"};
            expect(
                materialise(env.work, "mod", "refrain.test.manager", "1.0.0"));
            auto archive{pack(env.work, "mod", "out/mod.rfmod")};
            expect(archive.has_value());
            if (!archive) {
                return;
            }
            env.context.store = env.store_path();
            env.context.hot_install = false;
            auto service{env.service()};
            const auto report{
                run(service, {"install", *archive, "confirm"}, env.console())};
            expect(report.ok) << report.error;
            expect(report.outcome == "restart-required");
            expect(contains(report.lines, "restart required"));
            expect(env.receipt_count() == 1U);
            expect(env.audit.front().result == "restart-required");
        };

    "mod management/install with hot install but no lifecycle port is "
    "unsupported"_test = [] {
        fixture env{"install-unsupported"};
        expect(materialise(env.work, "mod", "refrain.test.manager", "1.0.0"));
        auto archive{pack(env.work, "mod", "out/mod.rfmod")};
        expect(archive.has_value());
        if (!archive) {
            return;
        }
        env.context.store = env.store_path();
        env.context.hot_install = true;
        env.context.lifecycle.unavailable_reason =
            "the hot lifecycle registry is not available in this build";
        auto service{env.service()};
        const auto report{
            run(service, {"install", *archive, "confirm"}, env.console())};
        expect(!report.ok);
        expect(report.outcome == "unsupported");
        expect(report.error.find("hot lifecycle registry") != std::string::npos)
            << report.error;
        // The store change is durable even though activation failed.
        expect(env.receipt_count() == 1U);
    };

    "mod management/trust lists the policy and the signer pins"_test = [] {
        fixture env{"trust-list"};
        env.context.policy.require_signature = true;
        env.context.policy.signature_policy =
            mod::rfmod_signature_policy::strict;
        const auto keypair{signed_keypair()};
        env.context.policy.trusted_keys.push_back(mod::rfmod_trusted_key{
            .key_id = std::string{key_a}, .public_key = keypair.public_key});
        expect(materialise(env.work, "mod", "refrain.test.manager", "1.0.0"));
        auto archive{
            pack(env.work, "mod", "out/mod.rfmod",
                 mod::rfmod_write_options{.signing = mod::rfmod_signing_options{
                                              .secret_key = keypair.secret_key,
                                              .key_id = std::string{key_a}}})};
        expect(archive.has_value());
        if (!archive) {
            return;
        }
        env.context.store = env.store_path();
        auto service{env.service()};
        expect(
            run(service, {"install", *archive, "confirm"}, env.console()).ok);
        env.audit.clear();
        const auto report{run(service, {"trust"}, env.console())};
        expect(report.ok) << report.error;
        expect(contains(report.lines, "trust anchor 'test-key-a'"));
        expect(contains(report.lines, "signer pin for refrain.test.manager"));
        expect(env.audit.size() == 1U);
        expect(env.audit.front().action == "trust-list");
    };

    "mod management/trust pin requires confirmation and writes the pin"_test =
        [] {
            fixture env{"trust-pin"};
            const auto keypair{signed_keypair()};
            env.context.policy.require_signature = true;
            env.context.policy.trusted_keys.push_back(
                mod::rfmod_trusted_key{.key_id = std::string{key_a},
                                       .public_key = keypair.public_key});
            expect(
                materialise(env.work, "mod", "refrain.test.manager", "1.0.0"));
            auto archive{pack(
                env.work, "mod", "out/mod.rfmod",
                mod::rfmod_write_options{.signing = mod::rfmod_signing_options{
                                             .secret_key = keypair.secret_key,
                                             .key_id = std::string{key_a}}})};
            expect(archive.has_value());
            if (!archive) {
                return;
            }
            env.context.store = env.store_path();
            auto service{env.service()};
            expect(run(service, {"install", *archive, "confirm"}, env.console())
                       .ok);
            expect(
                mod::clear_signer_pin(env.store_path(), "refrain.test.manager")
                    .has_value());
            env.audit.clear();
            const auto refused{run(service,
                                   {"trust", "pin", "refrain.test.manager"},
                                   env.console())};
            expect(!refused.ok);
            expect(refused.outcome == "refused");
            const auto pinned{run(
                service, {"trust", "pin", "refrain.test.manager", "confirm"},
                env.console())};
            expect(pinned.ok) << pinned.error;
            auto pin{
                mod::read_signer_pin(env.store_path(), "refrain.test.manager")};
            expect(static_cast<bool>(pin));
            if (pin && pin->has_value()) {
                expect((*pin)->key_id == key_a);
                expect((*pin)->source == "admin");
            }
            expect(env.audit.size() == 2U);
            expect(env.audit.back().result == "ok");
            expect(env.audit.back().key_id == key_a);
        };

    "mod management/unknown command and bad usage fail cleanly"_test = [] {
        fixture env{"usage"};
        auto service{env.service()};
        const auto unknown{run(service, {"frobnicate"}, env.console())};
        expect(!unknown.ok);
        expect(unknown.error.find("available mod commands") !=
               std::string::npos);
        const auto bad_option{run(service, {"list", "--force"}, env.console())};
        expect(!bad_option.ok);
        expect(bad_option.error.find("unknown option") != std::string::npos);
        const auto no_store{
            run(service, {"reload", "missing.mod"}, env.console())};
        expect(!no_store.ok);
        expect(no_store.error.find("running composition") != std::string::npos);
        // Every line, including a usage failure, is audited.
        expect(env.audit.size() == 3U);
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
