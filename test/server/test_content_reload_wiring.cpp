// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Content reload composition-root wiring: the running runtime takes the layer
// list and the validated initial view, the console `mods reload-data` command
// queues a rebuild through the shared command service, and the tick boundary
// publishes the new view as a new generation. The fixture is synthetic (a
// one-column flat world and a temporary datapack), so the assertions are about
// the wiring and the tick boundary, not about a gamepack.

#include <refrain/contracts/abi.hpp>
#include <refrain/core/io.hpp>
#include <refrain/core/log.hpp>
#include <refrain/data/content_view.hpp>
#include <refrain/server/content_reload_inputs.hpp>
#include <refrain/server/server_runtime.hpp>
#include <refrain/server/world_port.hpp>
#include <refrain/world/create.hpp>
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fmt/format.h>

#include "support/scratch_directory.hpp"

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace data = refrain::data;
namespace io = refrain::core::io;
namespace server = refrain::server;
namespace storage = refrain::storage;
namespace world_ns = refrain::world;
using refrain::test_support::scratch_directory;

constexpr std::string_view recipe_alpha{
    R"({"type":"minecraft:crafting_shapeless","group":"alpha"})"};
constexpr std::string_view recipe_beta{
    R"({"type":"minecraft:crafting_shapeless","group":"beta"})"};

[[nodiscard]] auto write_text(const std::filesystem::path &path,
                              std::string_view text) -> bool {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }
    return io::write_text(path.string(), text).has_value();
}

[[nodiscard]] auto read_text(const std::filesystem::path &path) -> std::string {
    std::ifstream in{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in},
                       std::istreambuf_iterator<char>{}};
}

[[nodiscard]] auto contains(const std::string &text, std::string_view needle)
    -> bool {
    return text.find(needle) != std::string::npos;
}

// --- flat world: one stone column at the spawn chunk -----------------------

[[nodiscard]] auto flat_chunk() -> storage::chunk {
    storage::chunk chunk{};
    chunk.format_version = world_ns::chunk_format_version;
    chunk.status = storage::chunk_status::full;
    chunk.y_pos = 0;
    storage::section_record section{};
    section.y = 0;
    section.kind = storage::section_kind::single_value;
    section.bits_per_entry = 0;
    section.block_count = static_cast<std::uint16_t>(world_ns::section_volume);
    section.biome_kind = storage::section_kind::single_value;
    section.biome_bits_per_entry = 0;
    section.biome_palette.push_back("minecraft:plains");
    section.palette.push_back(storage::palette_entry{
        .key = "minecraft:stone",
        .count = static_cast<std::uint16_t>(world_ns::section_volume),
        .state = 0,
    });
    chunk.sections.push_back(std::move(section));
    return chunk;
}

[[nodiscard]] auto create_flat_world(const std::filesystem::path &root)
    -> bool {
    if (!world_ns::create_world(root.string(), 0, "world")) {
        return false;
    }
    auto store{world_ns::world_store::open(root.string())};
    if (!store) {
        return false;
    }
    auto chunk{flat_chunk()};
    if (!store->write_chunk({0, 0}, chunk) || !store->commit()) {
        return false;
    }
    return true;
}

/// Handles that select the direct world port (dummy non-null vtables).
struct direct_handles {
    int dummy{0};
    server::server_service_handles handles{
        .world =
            reinterpret_cast<const refrain_services_world_vtable *>(&dummy),
        .world_self = &dummy,
        .sim = reinterpret_cast<const refrain_services_sim_vtable *>(&dummy),
        .sim_self = &dummy,
        .storage =
            reinterpret_cast<const refrain_services_storage_vtable *>(&dummy),
        .storage_self = &dummy,
        .world_provider = "refrain.world.vanilla",
        .sim_provider = "refrain.sim.vanilla",
        .storage_provider = "refrain.storage.rwf",
    };
};

/// Log fixture: one fresh file log per test.
struct log_capture {
    std::filesystem::path path{};
    bool running{false};

    explicit log_capture(const scratch_directory &scratch,
                         std::string_view name)
        : path{scratch.path() / (std::string{name} + ".log")} {
        static_cast<void>(io::write_text(path.string(), ""));
        refrain::log::shutdown();
        refrain::log::config settings{};
        settings.console_level = refrain::log::level::off;
        settings.file_path = path.string();
        settings.file_level = refrain::log::level::info;
        settings.enable_colors = false;
        expect(static_cast<bool>(refrain::log::init(settings)))
            << "log init failed";
        running = true;
    }
    log_capture(const log_capture &) = delete;
    auto operator=(const log_capture &) -> log_capture & = delete;
    ~log_capture() { stop(); }

    void stop() {
        if (running) {
            refrain::log::shutdown();
            running = false;
        }
    }

    [[nodiscard]] auto text() const -> std::string { return read_text(path); }
};

[[nodiscard]] auto write_config_dir(const scratch_directory &scratch,
                                    std::string_view name, bool hot_reload_data)
    -> std::string {
    const auto directory{scratch.path() / std::string{name}};
    const auto admin{fmt::format("[lifecycle]\nhot_reload_data = {}\n",
                                 hot_reload_data ? "true" : "false")};
    static_cast<void>(write_text(directory / "mods_admin.toml", admin));
    return directory.string();
}

[[nodiscard]] auto directory_source(const std::filesystem::path &root)
    -> data::content_source {
    return data::content_source{
        .kind = data::content_source_kind::directory,
        .path = root.string(),
        .label = root.string(),
    };
}

[[nodiscard]] auto build_initial_view(const std::filesystem::path &pack_dir)
    -> std::shared_ptr<const data::content_view> {
    const std::vector<data::content_source> sources{directory_source(pack_dir)};
    auto built{data::content_view::build(sources)};
    expect(static_cast<bool>(built)) << "initial view build failed";
    if (!built) {
        return nullptr;
    }
    return std::make_shared<const data::content_view>(std::move(*built));
}

[[nodiscard]] auto has_recipe(const data::content_view &view,
                              std::string_view name) -> bool {
    auto id{data::resource_location::parse("test:" + std::string{name})};
    return id && view.find(data::content_kind::recipe, *id) != nullptr;
}

/// What one tick observed through the published per-tick snapshot.
struct observations {
    std::uint64_t generation_at_tick_zero{0};
    bool alpha_at_tick_zero{false};
    bool beta_at_tick_zero{false};
    std::uint64_t generation_after{0};
    bool alpha_after{false};
    bool beta_after{false};
};

} // namespace

const suite content_reload_wiring_tests = [] {
    "content reload wiring/mods reload-data publishes a new generation at the "
    "tick boundary"_test = [] {
        scratch_directory scratch{};
        const auto pack_dir{scratch.path() / "pack"};
        expect(
            write_text(pack_dir / "data/test/recipe/alpha.json", recipe_alpha));
        auto initial{build_initial_view(pack_dir)};
        expect(initial != nullptr);
        if (!initial) {
            return;
        }
        // The file the operator changed after boot; it is only visible once a
        // rebuild publishes.
        expect(
            write_text(pack_dir / "data/test/recipe/beta.json", recipe_beta));
        const auto config_dir{write_config_dir(scratch, "config", true)};
        const auto world_path{scratch.path() / "world"};
        expect(create_flat_world(world_path)) << "flat world creation failed";
        const auto script{scratch.path() / "commands.txt"};
        expect(write_text(script, "0 mods reload-data\n"))
            << "script write failed";

        server::server_run_options options{};
        options.world_path = world_path.string();
        options.ticks = 6;
        options.script_file = script.string();
        options.config_dir = config_dir;
        observations seen{};
        options.on_tick = [&seen](std::uint64_t tick) {
            const auto live{server::published_content_view()};
            // on_tick reports the tick that just finished (1-based).
            if (tick == 1) {
                seen.generation_at_tick_zero = live.generation;
                if (live.view) {
                    seen.alpha_at_tick_zero = has_recipe(*live.view, "alpha");
                    seen.beta_at_tick_zero = has_recipe(*live.view, "beta");
                }
                return;
            }
            if (tick == 5 && live.view) {
                seen.generation_after = live.generation;
                seen.alpha_after = has_recipe(*live.view, "alpha");
                seen.beta_after = has_recipe(*live.view, "beta");
            }
        };
        server::publish_content_reload_inputs(server::content_reload_inputs{
            .sources = {directory_source(pack_dir)},
            .initial = initial,
        });
        log_capture log{scratch, "console"};
        direct_handles handles{};
        auto port{server::make_world_port(handles.handles)};
        expect(port != nullptr);
        if (!port) {
            return;
        }
        const auto exit_code{
            server::run_server(options, handles.handles, *port)};
        expect(exit_code == 0) << "run_server exit code " << exit_code;
        // The handoff was consumed by this run; a second runtime in the same
        // process cannot silently reuse the previous content.
        expect(!server::take_content_reload_inputs().has_value());
        // The command did not publish mid-tick: the view stayed generation 1
        // for the whole tick that queued the rebuild.
        expect(seen.generation_at_tick_zero == 1U);
        expect(seen.alpha_at_tick_zero);
        expect(!seen.beta_at_tick_zero);
        // The next tick boundary published the rebuilt view.
        expect(seen.generation_after == 2U);
        expect(seen.alpha_after);
        expect(seen.beta_after);
        log.stop();
        const auto text{log.text()};
        expect(contains(text, "queued a content view rebuild; generation 1 "
                              "stays live"))
            << text;
        expect(contains(text, "published content view generation 2")) << text;
        server::clear_content_view();
        server::clear_content_reload_inputs();
    };

    "content reload wiring/the hot gate off refuses the command and keeps the "
    "view"_test = [] {
        scratch_directory scratch{};
        const auto pack_dir{scratch.path() / "pack"};
        expect(
            write_text(pack_dir / "data/test/recipe/alpha.json", recipe_alpha));
        auto initial{build_initial_view(pack_dir)};
        expect(initial != nullptr);
        if (!initial) {
            return;
        }
        expect(
            write_text(pack_dir / "data/test/recipe/beta.json", recipe_beta));
        const auto config_dir{write_config_dir(scratch, "config", false)};
        const auto world_path{scratch.path() / "world"};
        expect(create_flat_world(world_path)) << "flat world creation failed";
        const auto script{scratch.path() / "commands.txt"};
        expect(write_text(script, "0 mods reload-data\n"))
            << "script write failed";
        server::server_run_options options{};
        options.world_path = world_path.string();
        options.ticks = 4;
        options.script_file = script.string();
        options.config_dir = config_dir;
        std::uint64_t last_generation{0};
        options.on_tick = [&last_generation](std::uint64_t tick) {
            if (tick == 3) {
                last_generation = server::published_content_view().generation;
            }
        };
        server::publish_content_reload_inputs(server::content_reload_inputs{
            .sources = {directory_source(pack_dir)},
            .initial = initial,
        });
        log_capture log{scratch, "console"};
        direct_handles handles{};
        auto port{server::make_world_port(handles.handles)};
        expect(port != nullptr);
        if (!port) {
            return;
        }
        const auto exit_code{
            server::run_server(options, handles.handles, *port)};
        expect(exit_code == 0) << "run_server exit code " << exit_code;
        expect(last_generation == 1U);
        log.stop();
        const auto text{log.text()};
        expect(contains(text, "hot content reload is disabled")) << text;
        expect(!contains(text, "published content view generation 2")) << text;
        server::clear_content_view();
        server::clear_content_reload_inputs();
    };
};
