// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Content view epoch: off-thread rebuild, validation rollback, registry
// generations and container refusals, hot gate and the non-blocking
// tick-boundary publish.

#include <refrain/app/content_reload.hpp>
#include <refrain/core/io.hpp>
#include <refrain/data/content_view.hpp>

#include <boost/ut.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "support/scratch_directory.hpp"

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace app = refrain::app;
namespace data = refrain::data;
namespace io = refrain::core::io;
using refrain::test_support::scratch_directory;

constexpr std::string_view recipe_alpha{
    R"({"type":"minecraft:crafting_shapeless","group":"alpha"})"};
constexpr std::string_view recipe_beta{
    R"({"type":"minecraft:crafting_shapeless","group":"beta"})"};

[[nodiscard]] auto write_file(const std::filesystem::path &root,
                              std::string_view relative, std::string_view text)
    -> bool {
    const auto target{root / std::filesystem::path{std::string{relative}}};
    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    if (error) {
        return false;
    }
    return io::write_text(target.string(), text).has_value();
}

[[nodiscard]] auto write_bytes(const std::filesystem::path &root,
                               std::string_view relative, std::string_view text)
    -> bool {
    const auto target{root / std::filesystem::path{std::string{relative}}};
    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    if (error) {
        return false;
    }
    return io::write_file(target.string(),
                          std::as_bytes(std::span{text.data(), text.size()}))
        .has_value();
}

[[nodiscard]] auto directory_source(const std::filesystem::path &root)
    -> data::content_source {
    return data::content_source{
        .kind = data::content_source_kind::directory,
        .path = root.string(),
        .label = root.string(),
    };
}

[[nodiscard]] auto build_view(const std::filesystem::path &root)
    -> refrain::result<std::shared_ptr<const data::content_view>> {
    const std::vector<data::content_source> sources{directory_source(root)};
    auto built{data::content_view::build(sources)};
    if (!built) {
        return std::unexpected{std::move(built.error())};
    }
    return std::make_shared<const data::content_view>(std::move(*built));
}

[[nodiscard]] auto has_recipe(const data::content_view &view,
                              std::string_view name,
                              std::string_view group = {}) -> bool {
    auto id{data::resource_location::parse("test:" + std::string{name})};
    if (!id) {
        return false;
    }
    const auto *entry{view.find(data::content_kind::recipe, *id)};
    if (entry == nullptr) {
        return false;
    }
    if (group.empty()) {
        return true;
    }
    const auto *node{entry->raw.find("group")};
    if (node == nullptr) {
        return false;
    }
    const auto text{node->as_string()};
    return text && *text == group;
}

[[nodiscard]] auto
wait_for_commit(app::content_reload &service,
                std::chrono::milliseconds timeout = std::chrono::seconds{10})
    -> app::content_reload_commit_result {
    const auto deadline{std::chrono::steady_clock::now() + timeout};
    app::content_reload_commit_result last{};
    do {
        last = service.commit();
        if (!last.pending) {
            return last;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    } while (std::chrono::steady_clock::now() < deadline);
    return last;
}

struct fixture {
    scratch_directory scratch{};
    std::filesystem::path pack_dir{scratch.path() / "pack"};

    fixture() {
        expect(
            write_file(pack_dir, "data/test/recipe/alpha.json", recipe_alpha));
    }

    [[nodiscard]] auto initial_view()
        -> std::shared_ptr<const data::content_view> {
        auto view{build_view(pack_dir)};
        expect(static_cast<bool>(view)) << "initial view build failed";
        return view ? *view : nullptr;
    }

    [[nodiscard]] auto enabled_options() -> app::content_reload_options {
        return app::content_reload_options{.enabled = true,
                                           .gate_source = "mods_admin.toml"};
    }
};

} // namespace

const suite content_reload_tests = [] {
    "content reload/publishes a rebuilt view at the boundary and keeps old "
    "readers alive"_test = [] {
        fixture f{};
        auto initial{f.initial_view()};
        expect(initial != nullptr);
        if (!initial) {
            return;
        }
        app::content_reload service{
            app::content_view_snapshot{.generation = 1, .view = initial},
            f.enabled_options(),
            [dir = f.pack_dir] { return build_view(dir); }};
        const auto before{service.snapshot()};
        expect(before.generation == 1U);
        expect(before.view != nullptr);
        if (!before.view) {
            return;
        }
        expect(has_recipe(*before.view, "alpha", "alpha"));
        const std::weak_ptr<const data::content_view> old_view{before.view};
        // A new recipe file appears in the same layer while the server runs.
        expect(
            write_file(f.pack_dir, "data/test/recipe/beta.json", recipe_beta));
        const auto requested{service.request()};
        expect(static_cast<bool>(requested));
        if (!requested) {
            return;
        }
        expect(!requested->empty());
        expect(requested->front().find("generation 1") != std::string::npos);
        // The live view does not change between the request and the publish.
        const auto committed{wait_for_commit(service)};
        expect(!committed.pending);
        expect(committed.published);
        expect(!committed.failed);
        expect(service.generation() == 2U);
        const auto after{service.snapshot()};
        expect(after.generation == 2U);
        expect(after.view != nullptr);
        if (after.view) {
            expect(has_recipe(*after.view, "alpha", "alpha"));
            expect(has_recipe(*after.view, "beta", "beta"));
        }
        // The reader that held generation 1 still sees the old data and keeps
        // the old view alive until it drops the snapshot.
        expect(!old_view.expired());
        expect(has_recipe(*before.view, "alpha", "alpha"));
        expect(!has_recipe(*before.view, "beta"));
        expect(before.view.use_count() >= 1);
    };

    "content reload/a reader releases the old view when it drops the "
    "snapshot"_test = [] {
        fixture f{};
        auto initial{f.initial_view()};
        expect(initial != nullptr);
        if (!initial) {
            return;
        }
        app::content_reload service{
            app::content_view_snapshot{.generation = 1, .view = initial},
            f.enabled_options(),
            [dir = f.pack_dir] { return build_view(dir); }};
        // Drop the test's own reference: only the service and its readers may
        // keep the view alive from here on.
        initial.reset();
        std::weak_ptr<const data::content_view> old_view{};
        {
            const auto held{service.snapshot()};
            old_view = held.view;
            expect(static_cast<bool>(service.request()));
            const auto committed{wait_for_commit(service)};
            expect(committed.published);
            expect(!old_view.expired());
        }
        // No reader and no live generation refers to the old view any more.
        expect(old_view.expired());
    };

    "content reload/a failed rebuild keeps the live view"_test = [] {
        fixture f{};
        auto initial{f.initial_view()};
        expect(initial != nullptr);
        if (!initial) {
            return;
        }
        app::content_reload service{
            app::content_view_snapshot{.generation = 1, .view = initial},
            f.enabled_options(),
            [dir = f.pack_dir] { return build_view(dir); }};
        expect(write_file(f.pack_dir, "data/test/recipe/beta.json",
                          R"({"type":})"));
        expect(static_cast<bool>(service.request()));
        const auto committed{wait_for_commit(service)};
        expect(!committed.pending);
        expect(committed.failed);
        expect(!committed.published);
        expect(!committed.restart_required);
        expect(committed.error.find("beta.json") != std::string::npos);
        expect(service.generation() == 1U);
        expect(service.snapshot().generation == 1U);
        if (const auto live{service.snapshot()}; live.view) {
            expect(has_recipe(*live.view, "alpha", "alpha"));
            expect(!has_recipe(*live.view, "beta"));
        }
        // The failure is not sticky: fixing the layer lets the next request
        // publish a fresh generation.
        expect(
            write_file(f.pack_dir, "data/test/recipe/beta.json", recipe_beta));
        expect(static_cast<bool>(service.request()));
        const auto retried{wait_for_commit(service)};
        expect(retried.published);
        expect(service.generation() == 2U);
    };

    "content reload/a registry document change publishes a new immutable "
    "generation"_test = [] {
        fixture f{};
        auto initial{f.initial_view()};
        expect(initial != nullptr);
        if (!initial) {
            return;
        }
        app::content_reload service{
            app::content_view_snapshot{.generation = 1, .view = initial},
            f.enabled_options(),
            [dir = f.pack_dir] { return build_view(dir); }};
        const auto before{service.snapshot()};
        expect(before.generation == 1U);
        expect(before.registry_generation == 1U);
        expect(before.registries != nullptr);
        if (before.registries != nullptr) {
            expect(before.registries->empty());
        }
        expect(write_file(f.pack_dir, "data/test/block/ruby.json",
                          R"({"type":"minecraft:block"})"));
        expect(static_cast<bool>(service.request()));
        const auto committed{wait_for_commit(service)};
        expect(!committed.pending);
        expect(committed.published);
        expect(!committed.failed);
        expect(!committed.restart_required);
        expect(committed.registry_generation_published);
        expect(service.generation() == 2U);
        expect(service.registry_generation() == 2U);
        const auto after{service.snapshot()};
        expect(after.generation == 2U);
        expect(after.registry_generation == 2U);
        expect(after.registries != nullptr);
        if (after.registries != nullptr) {
            expect(after.registries->size(data::registry_kind::block) == 1U);
        }
        // The snapshot taken before the publish keeps the generation it
        // started with: no reader observes a mapping swap under it.
        expect(before.generation == 1U);
        expect(before.registry_generation == 1U);
        expect(before.registries != nullptr);
        if (before.registries != nullptr) {
            expect(before.registries->empty());
        }
        expect(static_cast<bool>(committed.lines.size() >= 2U));
        bool named_registry_generation{false};
        for (const auto &line : committed.lines) {
            named_registry_generation =
                named_registry_generation ||
                line.find("registry generation 2") != std::string::npos;
        }
        expect(named_registry_generation);
        // A data-only reload afterwards keeps the registry generation; it is
        // published with the same mapping.
        expect(
            write_file(f.pack_dir, "data/test/recipe/beta.json", recipe_beta));
        expect(static_cast<bool>(service.request()));
        const auto data_only{wait_for_commit(service)};
        expect(data_only.published);
        expect(!data_only.registry_generation_published);
        expect(service.generation() == 3U);
        expect(service.registry_generation() == 2U);
        const auto last{service.snapshot()};
        expect(last.registries != nullptr);
        if (last.registries != nullptr) {
            expect(last.registries->size(data::registry_kind::block) == 1U);
        }
    };

    "content reload/a registry container change is refused as "
    "restart-required"_test = [] {
        fixture f{};
        auto initial{f.initial_view()};
        expect(initial != nullptr);
        if (!initial) {
            return;
        }
        app::content_reload service{
            app::content_view_snapshot{.generation = 1, .view = initial},
            f.enabled_options(),
            [dir = f.pack_dir] { return build_view(dir); }};
        expect(write_bytes(f.pack_dir, "tables.rvtb", "registry-table"));
        expect(static_cast<bool>(service.request()));
        const auto committed{wait_for_commit(service)};
        expect(!committed.pending);
        expect(committed.failed);
        expect(committed.restart_required);
        expect(!committed.published);
        expect(!committed.registry_generation_published);
        expect(committed.error.find("tables.rvtb") != std::string::npos);
        expect(committed.error.find("registry") != std::string::npos);
        expect(service.generation() == 1U);
        expect(service.registry_generation() == 1U);
        if (const auto live{service.snapshot()}; live.view) {
            expect(live.view->registry_inputs().empty());
        }
    };

    "content reload/a mixed registry change is refused as one unit"_test = [] {
        fixture f{};
        auto initial{f.initial_view()};
        expect(initial != nullptr);
        if (!initial) {
            return;
        }
        app::content_reload service{
            app::content_view_snapshot{.generation = 1, .view = initial},
            f.enabled_options(),
            [dir = f.pack_dir] { return build_view(dir); }};
        expect(write_file(f.pack_dir, "data/test/block/ruby.json",
                          R"({"type":"minecraft:block"})"));
        expect(write_bytes(f.pack_dir, "tables.rvtb", "registry-table"));
        expect(static_cast<bool>(service.request()));
        const auto committed{wait_for_commit(service)};
        expect(committed.failed);
        expect(committed.restart_required);
        expect(!committed.published);
        expect(!committed.registry_generation_published);
        // Neither half is applied: the error names both changed files and the
        // live view stays as it was.
        expect(committed.error.find("data/test/block/ruby.json") !=
               std::string::npos);
        expect(committed.error.find("tables.rvtb") != std::string::npos);
        expect(service.generation() == 1U);
        expect(service.registry_generation() == 1U);
        const auto live{service.snapshot()};
        expect(live.view != nullptr);
        if (live.view) {
            expect(live.view->registry_inputs().empty());
        }
        if (live.registries) {
            expect(live.registries->empty());
        }
    };

    "content reload/a disabled hot gate refuses the request"_test = [] {
        fixture f{};
        auto initial{f.initial_view()};
        expect(initial != nullptr);
        if (!initial) {
            return;
        }
        app::content_reload service{
            app::content_view_snapshot{.generation = 1, .view = initial},
            app::content_reload_options{.enabled = false,
                                        .gate_source = "mods_admin.toml"},
            [dir = f.pack_dir] { return build_view(dir); }};
        const auto requested{service.request()};
        expect(!requested);
        if (!requested) {
            expect(requested.error().code ==
                   refrain::error_code::permission_denied);
            expect(requested.error().message.find("hot_reload_data") !=
                   std::string::npos);
            expect(requested.error().message.find("mods_admin.toml") !=
                   std::string::npos);
        }
        expect(!service.pending());
        expect(service.generation() == 1U);
    };

    "content reload/commit never waits for a running build"_test = [] {
        fixture f{};
        auto initial{f.initial_view()};
        expect(initial != nullptr);
        if (!initial) {
            return;
        }
        auto started{std::make_shared<std::promise<void>>()};
        auto started_future{started->get_future().share()};
        auto release{std::make_shared<std::promise<void>>()};
        auto release_future{release->get_future().share()};
        app::content_reload service{
            app::content_view_snapshot{.generation = 1, .view = initial},
            f.enabled_options(),
            [dir = f.pack_dir, started, release = release_future]() {
                started->set_value();
                release.wait();
                return build_view(dir);
            }};
        expect(static_cast<bool>(service.request()));
        // The build has started and is blocked; the tick boundary must report
        // pending instead of waiting for it.
        expect(started_future.wait_for(std::chrono::seconds{5}) ==
               std::future_status::ready);
        const auto pending{service.commit()};
        expect(pending.pending);
        expect(!pending.published);
        expect(!pending.failed);
        expect(service.generation() == 1U);
        // A second reload request while one build is in flight is refused.
        const auto duplicate{service.request()};
        expect(!duplicate);
        if (!duplicate) {
            expect(duplicate.error().code ==
                   refrain::error_code::already_exists);
        }
        release->set_value();
        const auto published{wait_for_commit(service)};
        expect(published.published);
        expect(service.generation() == 2U);
    };

    "content reload/a digest-only registry change reports unchanged chunk "
    "output"_test = [] {
        fixture f{};
        expect(write_file(f.pack_dir, "data/test/block/ruby.json",
                          R"({"type":"minecraft:block","note":"v1"})"));
        auto initial{f.initial_view()};
        expect(initial != nullptr);
        if (!initial) {
            return;
        }
        app::content_reload service{
            app::content_view_snapshot{.generation = 1, .view = initial},
            f.enabled_options(),
            [dir = f.pack_dir] { return build_view(dir); }};
        // Changing the document digest without changing the entry name keeps
        // every name chunk generation resolves; the published line says the
        // chunk output is unchanged.
        expect(write_file(f.pack_dir, "data/test/block/ruby.json",
                          R"({"type":"minecraft:block","note":"v2"})"));
        expect(static_cast<bool>(service.request()));
        const auto committed{wait_for_commit(service)};
        expect(committed.published);
        expect(committed.registry_generation_published);
        expect(service.registry_generation() == 2U);
        bool names_unchanged{false};
        for (const auto &line : committed.lines) {
            names_unchanged =
                names_unchanged ||
                line.find("entry names are unchanged") != std::string::npos;
        }
        expect(names_unchanged);
        const auto live{service.snapshot()};
        expect(live.registries != nullptr);
        if (live.registries != nullptr) {
            expect(live.registries->size(data::registry_kind::block) == 1U);
        }
    };

    "content reload/refuses a registry generation the active path cannot "
    "bind"_test = [] {
        // The chunk-generation service reports what its path can bind; a
        // composition that selects the experimental scheduler publishes an
        // unsupported capability, and a document reload is refused.
        app::publish_registry_rebind_capability(app::registry_rebind_capability{
            .supported = false,
            .reason = "the experimental chunk scheduler keeps its boot content",
        });
        fixture f{};
        auto initial{f.initial_view()};
        expect(initial != nullptr);
        if (!initial) {
            app::clear_registry_rebind_capability();
            return;
        }
        app::content_reload service{
            app::content_view_snapshot{.generation = 1, .view = initial},
            f.enabled_options(),
            [dir = f.pack_dir] { return build_view(dir); }};
        expect(write_file(f.pack_dir, "data/test/block/ruby.json",
                          R"({"type":"minecraft:block"})"));
        expect(static_cast<bool>(service.request()));
        const auto committed{wait_for_commit(service)};
        expect(committed.failed);
        expect(committed.restart_required);
        expect(!committed.published);
        expect(!committed.registry_generation_published);
        expect(committed.error.find("data/test/block/ruby.json") !=
               std::string::npos);
        bool named_reason{false};
        for (const auto &line : committed.lines) {
            named_reason =
                named_reason ||
                line.find("experimental chunk scheduler") != std::string::npos;
        }
        expect(named_reason);
        expect(service.generation() == 1U);
        expect(service.registry_generation() == 1U);
        app::clear_registry_rebind_capability();
    };

    "content reload/refuses a changed registry document that cannot define an "
    "entry"_test = [] {
        fixture f{};
        auto initial{f.initial_view()};
        expect(initial != nullptr);
        if (!initial) {
            return;
        }
        app::content_reload service{
            app::content_view_snapshot{.generation = 1, .view = initial},
            f.enabled_options(),
            [dir = f.pack_dir] { return build_view(dir); }};
        // A non-JSON file under a registry directory cannot name an entry. The
        // refusal says so instead of calling the path a registry container.
        expect(write_file(f.pack_dir, "data/test/block/notes", "notes"));
        expect(static_cast<bool>(service.request()));
        const auto committed{wait_for_commit(service)};
        expect(committed.failed);
        expect(committed.restart_required);
        expect(!committed.published);
        expect(committed.error.find("cannot define a registry entry") !=
               std::string::npos);
        expect(committed.error.find("data/test/block/notes") !=
               std::string::npos);
        expect(service.generation() == 1U);
        expect(service.registry_generation() == 1U);
    };
};
