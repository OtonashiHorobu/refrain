// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/app/mod_lifecycle.hpp>

#include <refrain/core/log.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/package.hpp>

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace refrain::app {
namespace {

constexpr refrain::log::logger log_lifecycle{"mod-lifecycle"};

/// Process-wide tick-boundary targets. The runtime has one server, but tests
/// (and embedders) construct isolated controllers, so the list is explicit
/// rather than a singleton.
struct tick_hook_state {
    std::mutex mutex{};
    std::vector<mod_lifecycle *> controllers{};
};

[[nodiscard]] auto tick_hooks() -> tick_hook_state & {
    static tick_hook_state state{};
    return state;
}

[[nodiscard]] auto describe(const mod::staged_composition_change &change)
    -> std::string {
    std::string out;
    const auto append = [&out](std::string_view label,
                               const std::vector<std::string> &ids) {
        if (ids.empty()) {
            return;
        }
        out +=
            fmt::format("{}{} {}", out.empty() ? "" : ", ", label, ids.size());
        out += " (";
        for (std::size_t index{0}; index < ids.size(); ++index) {
            if (index != 0U) {
                out += ", ";
            }
            out += ids[index];
        }
        out += ')';
    };
    append("added", change.added);
    append("reloaded", change.reloaded);
    append("removed", change.removed);
    if (out.empty()) {
        out = "no mod changes";
    }
    return out;
}

} // namespace

mod_lifecycle::mod_lifecycle(mod::host &owner, mod_lifecycle_options options)
    : owner_{owner}, options_{std::move(options)} {
    attach_mod_lifecycle(*this);
}

mod_lifecycle::~mod_lifecycle() {
    detach_mod_lifecycle(*this);
    if (pending()) {
        abort();
    }
}

auto mod_lifecycle::primary_store() -> result<std::string> {
    if (!options_.store_root.empty()) {
        return options_.store_root;
    }
    if (!options_.store_roots.empty()) {
        return options_.store_roots.front();
    }
    return std::unexpected{make_error(
        error_code::not_initialized,
        "hot mod management has no content store configured; pass the server "
        "--mods store root to the lifecycle service")};
}

auto mod_lifecycle::resolve_composition() -> result<mod::composition> {
    auto required{options_.required_services |
                  std::ranges::to<std::vector<std::string_view>>()};
    // A caller may configure only the write store; the same store is then the
    // catalog root, otherwise an install would write a receipt the running
    // composition never resolves.
    std::vector<std::string> roots{options_.store_roots};
    if (roots.empty() && !options_.store_root.empty()) {
        roots.push_back(options_.store_root);
    }
    auto composition{mod::resolve_installed_composition(
        owner_.active_composition().spec, roots,
        mod::composition_load_options{
            .builtin_image = owner_.builtin_image_bytes(),
            .host_side = options_.side,
            .required_services = required,
            .discovery = options_.discovery,
            .auto_scan = options_.auto_scan,
        })};
    if (!composition) {
        return std::unexpected{std::move(composition.error())};
    }
    return composition;
}

auto mod_lifecycle::stage_resolved() -> result<mod_operation> {
    auto next{resolve_composition()};
    if (!next) {
        return std::unexpected{std::move(next.error())};
    }
    auto resolver{mod::make_package_resolver(owner_.builtin_image_bytes())};
    if (!resolver) {
        return std::unexpected{std::move(resolver.error())};
    }
    auto change{owner_.stage_composition(
        *next, *resolver,
        mod::stage_options{.drain_timeout = options_.drain_timeout})};
    if (!change) {
        return std::unexpected{std::move(change.error())};
    }
    return mod_operation{.action = "install", .change = std::move(*change)};
}

auto mod_lifecycle::install(std::string_view archive_path,
                            const mod::rfmod_install_options &install_options)
    -> result<mod_operation> {
    auto store_root{primary_store()};
    if (!store_root) {
        return std::unexpected{std::move(store_root.error())};
    }
    if (!install_options.write_receipt) {
        return std::unexpected{make_error(
            error_code::invalid_argument,
            "hot install requires a receipt (write_receipt must stay enabled): "
            "the receipt is the only runtime load source, so an install that "
            "records none could never be loaded by the running composition")};
    }
    if (owner_.staged_change().has_value()) {
        return std::unexpected{make_error(
            error_code::already_exists,
            "another hot mod change is already staged; commit or abort it "
            "first")};
    }
    auto store{data::content_store::open_existing(*store_root)};
    if (!store) {
        return std::unexpected{std::move(store.error())};
    }
    auto reader{mod::rfmod_reader::open_file(archive_path)};
    if (!reader) {
        return std::unexpected{std::move(reader.error())};
    }
    log_lifecycle.info("installing package {} into content store '{}'",
                       archive_path, *store_root);
    auto installed{mod::install_rfmod(*reader, *store, install_options)};
    if (!installed) {
        return std::unexpected{std::move(installed.error())};
    }
    auto operation{stage_resolved()};
    if (!operation) {
        // The install transaction is already committed: keeping the receipt is
        // what the next server start reads, so a refused hot switch must not
        // roll the store back.
        log_lifecycle.warn(
            "installed {}@{} but could not stage it in the running "
            "composition: {}; it stays installed for the next server start",
            installed->manifest.common.id, installed->manifest.common.version,
            operation.error().message);
        return operation;
    }
    operation->action = "install";
    operation->detail =
        fmt::format("installed {}@{} from {}", installed->manifest.common.id,
                    installed->manifest.common.version, archive_path);
    log_lifecycle.info(
        "staged install of {}@{} generation {} for the next tick, {}",
        installed->manifest.common.id, installed->manifest.common.version,
        owner_.find(installed->manifest.common.id) == nullptr
            ? 1
            : owner_.find(installed->manifest.common.id)->generation + 1,
        describe(operation->change));
    return operation;
}

auto mod_lifecycle::uninstall(std::string_view id, std::string_view version)
    -> result<mod_operation> {
    if (owner_.staged_change().has_value()) {
        return std::unexpected{make_error(
            error_code::already_exists,
            "another hot mod change is already staged; commit or abort it "
            "first")};
    }
    const auto *installed{owner_.find(id)};
    if (installed == nullptr) {
        return std::unexpected{make_error(
            error_code::not_found,
            fmt::format("mod '{}' is not part of the running composition",
                        id))};
    }
    if (!version.empty() && version != installed->version) {
        return std::unexpected{make_error(
            error_code::version_mismatch,
            fmt::format("the running composition has '{}@{}', not '{}'", id,
                        installed->version, version))};
    }
    auto change{owner_.stage_uninstall(
        id, mod::stage_options{.drain_timeout = options_.drain_timeout})};
    if (!change) {
        return std::unexpected{std::move(change.error())};
    }
    mod_operation operation{.action = "uninstall",
                            .change = std::move(*change)};
    operation.detail =
        fmt::format("staged removal of {}@{}", id, installed->version);
    // The composition no longer references the mod once the commit publishes
    // the change; the receipt is removed now because the command runs outside
    // the tick and the tick must not do store IO.
    if (installed->external) {
        auto store_root{primary_store()};
        if (!store_root) {
            owner_.abort_staged();
            return std::unexpected{std::move(store_root.error())};
        }
        auto store{data::content_store::open_existing(*store_root)};
        if (!store) {
            owner_.abort_staged();
            return std::unexpected{std::move(store.error())};
        }
        std::optional<std::string_view> requested{};
        if (!version.empty()) {
            requested = version;
        }
        auto removed{mod::uninstall_rfmod(*store, id, requested)};
        if (!removed) {
            owner_.abort_staged();
            return std::unexpected{std::move(removed.error())};
        }
        for (const auto &removed_version : removed->versions) {
            log_lifecycle.info("removed install receipt {}@{}", removed->id,
                               removed_version);
        }
        operation.gc_hint =
            fmt::format("run `refrain-tools content gc --store {} --apply` to "
                        "collect the unreferenced package blobs",
                        *store_root);
    }
    log_lifecycle.info("staged uninstall of {}@{} for the next tick, {}", id,
                       installed->version, describe(operation.change));
    return operation;
}

auto mod_lifecycle::stage_catalog() -> result<mod_operation> {
    if (owner_.staged_change().has_value()) {
        return std::unexpected{make_error(
            error_code::already_exists,
            "another hot mod change is already staged; commit or abort it "
            "first")};
    }
    auto operation{stage_resolved()};
    if (!operation) {
        return operation;
    }
    operation->action = "catalog";
    operation->detail = "staged catalog refresh";
    log_lifecycle.info("staged catalog refresh for the next tick, {}",
                       describe(operation->change));
    return operation;
}

auto mod_lifecycle::stage_removal(std::string_view id, std::string_view version)
    -> result<mod_operation> {
    if (owner_.staged_change().has_value()) {
        return std::unexpected{make_error(
            error_code::already_exists,
            "another hot mod change is already staged; commit or abort it "
            "first")};
    }
    const auto *installed{owner_.find(id)};
    if (installed == nullptr) {
        return std::unexpected{make_error(
            error_code::not_found,
            fmt::format("mod '{}' is not part of the running composition",
                        id))};
    }
    if (!version.empty() && version != installed->version) {
        return std::unexpected{make_error(
            error_code::version_mismatch,
            fmt::format("the running composition has '{}@{}', not '{}'", id,
                        installed->version, version))};
    }
    auto change{owner_.stage_uninstall(
        id, mod::stage_options{.drain_timeout = options_.drain_timeout})};
    if (!change) {
        return std::unexpected{std::move(change.error())};
    }
    mod_operation operation{.action = "uninstall",
                            .change = std::move(*change)};
    operation.detail =
        fmt::format("staged removal of {}@{}", id, installed->version);
    log_lifecycle.info("staged uninstall of {}@{} for the next tick, {}", id,
                       installed->version, describe(operation.change));
    return operation;
}

auto mod_lifecycle::reload(std::string_view id) -> result<mod_operation> {
    if (owner_.staged_change().has_value()) {
        return std::unexpected{make_error(
            error_code::already_exists,
            "another hot mod change is already staged; commit or abort it "
            "first")};
    }
    const auto *installed{owner_.find(id)};
    if (installed == nullptr) {
        return std::unexpected{make_error(
            error_code::not_found,
            fmt::format("mod '{}' is not part of the running composition",
                        id))};
    }
    auto resolver{mod::make_package_resolver(owner_.builtin_image_bytes())};
    if (!resolver) {
        return std::unexpected{std::move(resolver.error())};
    }
    auto change{owner_.stage_composition(
        owner_.active_composition(), *resolver,
        mod::stage_options{.reload = {std::string{id}},
                           .drain_timeout = options_.drain_timeout})};
    if (!change) {
        return std::unexpected{std::move(change.error())};
    }
    mod_operation operation{.action = "reload", .change = std::move(*change)};
    operation.detail =
        fmt::format("staged reload of {}@{} as generation {}", id,
                    installed->version, installed->generation + 1);
    log_lifecycle.info("staged reload of {}@{} for the next tick, {}", id,
                       installed->version, describe(operation.change));
    return operation;
}

auto mod_lifecycle::commit() -> result<mod_commit_result> {
    if (!pending()) {
        return mod_commit_result{.commit = mod::composition_commit{
                                     .epoch = owner_.epoch(),
                                     .digest = owner_.composition_digest(),
                                 }};
    }
    auto committed{owner_.commit_epoch()};
    if (!committed) {
        return std::unexpected{std::move(committed.error())};
    }
    log_lifecycle.info(
        "commit by {} published composition epoch {} with {} added, {} "
        "reloaded and {} removed mod(s), digest {}, {} retired native "
        "mapping(s)",
        options_.actor, committed->epoch, committed->added.size(),
        committed->reloaded.size(), committed->removed.size(),
        committed->digest, committed->retired_native_generations);
    for (const auto &id : committed->quarantined) {
        log_lifecycle.error(
            "mod {} was quarantined during the composition commit and is never "
            "called again until the server restarts",
            id);
    }
    return mod_commit_result{.commit = std::move(*committed)};
}

void mod_lifecycle::abort() noexcept { owner_.abort_staged(); }

namespace {

[[nodiscard]] auto lists_id(const std::vector<std::string> &ids,
                            std::string_view id) -> bool {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

} // namespace

auto make_mod_lifecycle_port(mod_lifecycle &lifecycle) -> mod_lifecycle_port {
    mod_lifecycle_port port{};
    port.apply = [&lifecycle](const mod_lifecycle_request &request)
        -> result<mod_lifecycle_outcome> {
        const auto describe_change = [](std::string_view what,
                                        const mod_operation &operation) {
            std::string line{fmt::format("staged {} for the next tick", what)};
            if (!operation.change.added.empty()) {
                line += "; adds " +
                        std::to_string(operation.change.added.size()) +
                        " mod(s)";
            }
            if (!operation.change.reloaded.empty()) {
                line += "; reloads " +
                        std::to_string(operation.change.reloaded.size()) +
                        " mod(s)";
            }
            if (!operation.change.removed.empty()) {
                line += "; removes " +
                        std::to_string(operation.change.removed.size()) +
                        " mod(s)";
            }
            return line;
        };
        result<mod_operation> staged = std::unexpected{make_error(
            error_code::invalid_argument, "unknown lifecycle action")};
        switch (request.action) {
        case mod_lifecycle_action::install:
            staged = lifecycle.stage_catalog();
            if (staged && !request.id.empty() &&
                lifecycle.host().find(request.id) == nullptr &&
                !lists_id(staged->change.added, request.id) &&
                !lists_id(staged->change.reloaded, request.id)) {
                // The command layer committed the receipt, but the catalog
                // rules the running composition was resolved with do not
                // select the mod, so the staged change is empty. Publishing
                // it would report a composition change that did not happen;
                // drop it and name the reason instead.
                lifecycle.abort();
                return std::unexpected{make_error(
                    error_code::unsupported,
                    fmt::format(
                        "the install receipt of '{}' is not part of the "
                        "resolved composition: the gamepack does not select "
                        "it and auto-scan is off, so the running composition "
                        "cannot include it; enable auto-scan or list the mod "
                        "in the gamepack and restart the server",
                        request.id))};
            }
            break;
        case mod_lifecycle_action::uninstall:
            staged = lifecycle.stage_removal(request.id, request.version);
            break;
        case mod_lifecycle_action::reload:
            staged = lifecycle.reload(request.id);
            break;
        }
        if (!staged) {
            return std::unexpected{std::move(staged.error())};
        }
        mod_lifecycle_outcome outcome{};
        outcome.lines.push_back(describe_change(
            request.id.empty() ? "the catalog change" : request.id, *staged));
        outcome.lines.push_back(fmt::format(
            "the running composition stays on epoch {} until the next tick "
            "boundary publishes the staged change",
            lifecycle.host().epoch()));
        return outcome;
    };
    return port;
}

void attach_mod_lifecycle(mod_lifecycle &controller) {
    auto &hooks{tick_hooks()};
    const std::lock_guard lock{hooks.mutex};
    if (std::find(hooks.controllers.begin(), hooks.controllers.end(),
                  &controller) == hooks.controllers.end()) {
        hooks.controllers.push_back(&controller);
    }
}

void detach_mod_lifecycle(mod_lifecycle &controller) noexcept {
    try {
        auto &hooks{tick_hooks()};
        const std::lock_guard lock{hooks.mutex};
        std::erase(hooks.controllers, &controller);
    } catch (...) {
        // Detaching must not throw out of a destructor.
    }
}

auto commit_staged_mod_changes() -> status {
    std::vector<mod_lifecycle *> controllers{};
    {
        auto &hooks{tick_hooks()};
        const std::lock_guard lock{hooks.mutex};
        controllers = hooks.controllers;
    }
    status first_failure{};
    for (auto *controller : controllers) {
        if (controller == nullptr || !controller->pending()) {
            continue;
        }
        auto committed{controller->commit()};
        if (!committed && first_failure) {
            first_failure = std::unexpected{std::move(committed.error())};
        }
    }
    return first_failure;
}

} // namespace refrain::app
