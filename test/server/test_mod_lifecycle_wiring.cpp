// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Hot lifecycle composition-root wiring: a running server runtime builds the
// lifecycle service from the host and the catalog it was given, the console
// `mods ...` frontend stages a change through the shared command service and
// the tick boundary publishes it. The fixture is synthetic (a leaf mod behind
// a clean fake loader, a temporary content store and a one-column flat world)
// so the assertions are about the wiring, not about a gamepack.

#include <refrain/app/host_context.hpp>
#include <refrain/app/mod_lifecycle.hpp>
#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/mod_loader_abi.h>
#include <refrain/core/config.hpp>
#include <refrain/core/io.hpp>
#include <refrain/core/log.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/host/core_api.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/package.hpp>
#include <refrain/mod/rfmod.hpp>
#include <refrain/mod/static_mod.hpp>
#include <refrain/server/mod_lifecycle_inputs.hpp>
#include <refrain/server/server_runtime.hpp>
#include <refrain/server/world_port.hpp>
#include <refrain/sim/world.hpp>
#include <refrain/world/create.hpp>
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "support/scratch_directory.hpp"

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace config = refrain::core::config;
namespace data = refrain::data;
namespace io = refrain::core::io;
namespace kernel = refrain::kernel;
namespace mod = refrain::mod;
namespace server = refrain::server;
namespace sim = refrain::sim;
namespace storage = refrain::storage;
namespace world_ns = refrain::world;

using refrain::test_support::scratch_directory;

// ---------------------------------------------------------------------------
// synthetic package helpers
// ---------------------------------------------------------------------------

constexpr std::string_view clean_loader_id{"com.example.clean"};

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

/// One external mod manifest: every fixture mod runs through the clean fake
/// loader, and declares the service it wants to provide.
[[nodiscard]] auto manifest_text(std::string_view id, std::string_view provides)
    -> std::string {
    return fmt::format("[mod]\n"
                       "id = \"{}\"\n"
                       "version = \"1.0.0\"\n"
                       "abi = \"1\"\n"
                       "side = \"server\"\n"
                       "\n[provides]\n"
                       "services = [\"{}\"]\n"
                       "\n[entry]\n"
                       "loader = \"{}\"\n",
                       id, provides, clean_loader_id);
}

/// Pack one mod directory into an `.rfmod` archive under the scratch tree.
[[nodiscard]] auto pack_mod(const scratch_directory &scratch,
                            std::string_view directory,
                            std::string_view manifest)
    -> std::optional<std::string> {
    const auto mod_dir{scratch.path() / std::string{directory}};
    if (!write_text(mod_dir / "manifest.toml", manifest) ||
        !write_text(mod_dir / "data/test/payload.json",
                    R"({"payload":"alpha"})")) {
        return std::nullopt;
    }
    const auto archive{scratch.path() / (std::string{directory} + ".rfmod")};
    auto packed{mod::pack_mod_directory(mod_dir.string(), archive.string())};
    expect(static_cast<bool>(packed)) << "pack_mod_directory failed";
    if (!packed) {
        return std::nullopt;
    }
    return archive.string();
}

/// Open (creating when absent) the content store and install one archive.
[[nodiscard]] auto install_archive(const scratch_directory &scratch,
                                   const std::string &archive,
                                   std::string_view store_name)
    -> std::optional<std::string> {
    const auto store_root{scratch.path() / std::string{store_name}};
    auto store{data::content_store::open(store_root.string())};
    expect(static_cast<bool>(store)) << "content_store::open failed";
    if (!store) {
        return std::nullopt;
    }
    auto reader{mod::rfmod_reader::open_file(archive)};
    expect(static_cast<bool>(reader)) << "rfmod_reader::open_file failed";
    if (!reader) {
        return std::nullopt;
    }
    auto installed{mod::install_rfmod(*reader, *store)};
    expect(static_cast<bool>(installed)) << "install_rfmod failed";
    if (!installed) {
        return std::nullopt;
    }
    return store_root.string();
}

/// The runtime reads the install/trust policy from `config/mods.toml` and the
/// hot gates and operator list from `config/mods_admin.toml`.
[[nodiscard]] auto write_config_dir(const scratch_directory &scratch,
                                    std::string_view name, bool hot_install,
                                    bool hot_reload) -> std::string {
    const auto directory{scratch.path() / std::string{name}};
    const auto admin{fmt::format(
        "[lifecycle]\nhot_install = {}\nhot_reload = {}\n"
        "\n[admin]\noperators = []\n",
        hot_install ? "true" : "false", hot_reload ? "true" : "false")};
    // The fixture packages are unsigned; allow unsigned installs.
    static_cast<void>(write_text(directory / "mods_admin.toml", admin));
    static_cast<void>(
        write_text(directory / "mods.toml", "[signature]\nrequire = false\n"));
    return directory.string();
}

[[nodiscard]] auto parse_spec(std::string_view text)
    -> refrain::result<mod::gamepack_spec> {
    auto document{config::document::parse_text(text, "<gamepack>")};
    if (!document) {
        return std::unexpected{document.error()};
    }
    return mod::parse_gamepack(*document, "server");
}

// ---------------------------------------------------------------------------
// clean fake loader: a sandbox-free loader that declares `clean` unload, the
// stand-in for the script loaders this host does not build. A code mod
// registers the guest service; a data-only load registers nothing.
// ---------------------------------------------------------------------------

std::uint32_t g_clean_vtable[4]{16, 1, 0, 0};

struct clean_loader_state {
    mod::host *owner{nullptr};
    std::string loader_id{clean_loader_id};
    std::string guest_service{"refrain:leaf"};
    int load_calls{0};
    int reload_calls{0};
    int unload_calls{0};
    std::uint64_t next_handle{1};
    std::vector<std::unique_ptr<int>> selves{};

    [[nodiscard]] auto register_guest() -> bool {
        auto self{std::make_unique<int>(0)};
        *self = static_cast<int>(selves.size() + 1U);
        const mod::provider_api api{
            .vtable = g_clean_vtable,
            .self = self.get(),
            .abi_major = 1,
        };
        selves.push_back(std::move(self));
        return owner->register_provider(guest_service, api).has_value();
    }
};

void clean_capabilities(
    void *self, refrain_services_mod_loader_mod_loader_capabilities *out) {
    auto &state{*static_cast<clean_loader_state *>(self)};
    out->struct_size = REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_CAPABILITIES_SIZE;
    out->loader_id = refrain::contracts::to_borrowed(state.loader_id);
    out->abi_major = REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR;
    out->unload = REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_UNLOAD_CLEAN;
    out->sandboxed = 0;
    out->supports_hot_install = 1;
}

auto clean_validate(void *,
                    const refrain_services_mod_loader_mod_entry_manifest *)
    -> refrain_status {
    return REFRAIN_STATUS_OK;
}

auto clean_load(void *self,
                const refrain_services_mod_loader_mod_load_request *request,
                refrain_services_mod_loader_handle_mod_instance *out)
    -> refrain_status {
    auto &state{*static_cast<clean_loader_state *>(self)};
    ++state.load_calls;
    *out = REFRAIN_SERVICES_MOD_LOADER_HANDLE_MOD_INSTANCE_INVALID;
    if (request == nullptr || state.owner == nullptr) {
        return static_cast<refrain_status>(
            REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_INVALID_ARGUMENT);
    }
    if (request->manifest.activate_code == 0) {
        return REFRAIN_STATUS_OK;
    }
    if (!state.register_guest()) {
        return static_cast<refrain_status>(
            REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_PERMISSION_DENIED);
    }
    *out = state.next_handle++;
    return REFRAIN_STATUS_OK;
}

auto clean_reload(void *self,
                  refrain_services_mod_loader_handle_mod_instance instance,
                  const refrain_services_mod_loader_mod_load_request *request,
                  refrain_services_mod_loader_handle_mod_instance *out)
    -> refrain_status {
    auto &state{*static_cast<clean_loader_state *>(self)};
    ++state.reload_calls;
    if (request == nullptr || state.owner == nullptr ||
        instance == REFRAIN_SERVICES_MOD_LOADER_HANDLE_MOD_INSTANCE_INVALID) {
        return static_cast<refrain_status>(
            REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_INVALID_ARGUMENT);
    }
    if (request->manifest.activate_code != 0 && !state.register_guest()) {
        return static_cast<refrain_status>(
            REFRAIN_SERVICES_MOD_LOADER_MOD_LOADER_ERROR_PERMISSION_DENIED);
    }
    *out = state.next_handle++;
    return REFRAIN_STATUS_OK;
}

auto clean_unload(void *self, refrain_services_mod_loader_handle_mod_instance,
                  refrain_services_mod_loader_mod_unload_reason)
    -> refrain_status {
    ++static_cast<clean_loader_state *>(self)->unload_calls;
    return REFRAIN_STATUS_OK;
}

[[nodiscard]] auto clean_loader_vtable()
    -> const refrain_services_mod_loader_vtable & {
    static const refrain_services_mod_loader_vtable table{
        .struct_size = REFRAIN_SERVICES_MOD_LOADER_VTABLE_SIZE,
        .abi_major = REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR,
        .abi_minor = REFRAIN_SERVICES_MOD_LOADER_ABI_MINOR,
        .abi_patch = REFRAIN_SERVICES_MOD_LOADER_ABI_PATCH,
        .capabilities = &clean_capabilities,
        .validate = &clean_validate,
        .load = &clean_load,
        .unload = &clean_unload,
        .reload = &clean_reload,
    };
    return table;
}

[[nodiscard]] auto register_clean_loader(mod::host &host,
                                         clean_loader_state &state) -> bool {
    state.owner = &host;
    const mod::provider_api api{
        .vtable = &clean_loader_vtable(),
        .self = &state,
        .abi_major = REFRAIN_SERVICES_MOD_LOADER_ABI_MAJOR,
    };
    return host.register_mod_loader(clean_loader_id, api).has_value();
}

/// A loaded synthetic host with its own static registry, so the real
/// first-party registry of the test binary is untouched.
struct wired_host {
    mod::static_mod_registry registry{};
    // Declared before the host so it outlives it: a loader hook must never run
    // against a destroyed state object.
    clean_loader_state loader{};
    mod::host host{registry};
    // The sealed `refrain:host.core` context a mod reaches the registry
    // through, declared after the host it reads.
    refrain::app::host_context context{host};
};

/// Resolve the installed catalog against `roots` and load it into `fixture`.
[[nodiscard]] auto load_host(wired_host &fixture,
                             const mod::gamepack_spec &spec,
                             const std::vector<std::string> &roots) -> bool {
    auto plan{mod::resolve_installed_composition(
        spec, roots, mod::composition_load_options{.host_side = "server"})};
    expect(static_cast<bool>(plan))
        << (plan ? std::string{"resolved"} : plan.error().message);
    if (!plan) {
        return false;
    }
    if (!register_clean_loader(fixture.host, fixture.loader)) {
        expect(false) << "clean loader registration failed";
        return false;
    }
    const mod::provider_api host_core_api{
        .vtable = &refrain::host::core_vtable(),
        .self = &fixture.context,
        .abi_major = REFRAIN_HOST_CORE_ABI_MAJOR,
    };
    if (!fixture.host
             .register_sealed_provider("refrain:host.core", host_core_api,
                                       "1.1.0")
             .has_value()) {
        expect(false) << "sealed host core registration failed";
        return false;
    }
    auto loaded{fixture.host.load(*plan, std::span<const std::byte>{})};
    expect(static_cast<bool>(loaded))
        << (loaded ? std::string{"load ok"} : loaded.error().message);
    return loaded.has_value();
}

// ---------------------------------------------------------------------------
// flat world: one stone column at the spawn chunk, so the runtime boots
// without generating terrain.
// ---------------------------------------------------------------------------

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

/// Handles that select the direct world port. The runtime never routes a call
/// through the vtables on that path, so non-null dummy pointers carry the
/// selection and the provider ids name the first-party implementations.
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

/// Log fixture: start a fresh file log for one test, stop it afterwards.
/// The file sink is buffered, so callers call `stop()` before reading the file;
/// the destructor only stops a log that is still running.
struct log_capture {
    std::filesystem::path path{};
    bool running{false};

    explicit log_capture(const scratch_directory &scratch,
                         std::string_view name)
        : path{scratch.path() / (std::string{name} + ".log")} {
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

[[nodiscard]] auto contains(const std::string &text, std::string_view needle)
    -> bool {
    return text.find(needle) != std::string::npos;
}

/// The leaf fixture used by every test: a code provider behind the clean
/// loader.
constexpr std::string_view leaf_id{"refrain.test.wire.leaf"};
constexpr std::string_view core_id{"refrain.test.wire.core"};

[[nodiscard]] auto pinned_spec(std::string_view mod_id,
                               std::string_view provider_key,
                               bool auto_scan = true) -> std::string {
    return fmt::format("[gamepack]\n"
                       "id = \"refrain.test.wire\"\n"
                       "version = \"1.0.0\"\n"
                       "side = \"server\"\n"
                       "abi = \"1\"\n"
                       "auto_scan = {}\n"
                       "\n[mods]\n"
                       "list = [\"{}@1.0.0\"]\n"
                       "\n[providers]\n"
                       "{} = \"{}@^1.0.0\"\n",
                       auto_scan ? "true" : "false", mod_id, provider_key,
                       mod_id);
}

constexpr std::string_view empty_spec_text{"[gamepack]\n"
                                           "id = \"refrain.test.wire\"\n"
                                           "version = \"1.0.0\"\n"
                                           "side = \"server\"\n"
                                           "abi = \"1\"\n"
                                           "auto_scan = true\n"
                                           "\n[mods]\n"
                                           "list = []\n"};

/// What the tick hooks observed; the commit happens at the start of the tick
/// after the command that staged the change.
struct observations {
    std::uint64_t epoch_before_commit{0};
    bool staged_before_commit{false};
    std::optional<kernel::provider_handle> oldest_handle{};
    std::uint64_t epoch_after_commit{0};
    std::uint64_t generation_after_commit{0};
    std::optional<kernel::provider_handle> new_handle{};
    std::uint64_t epoch_after_removal{0};
    bool composition_without_mod{false};
    /// The same slots as a mod sees them through the sealed host core ABI: one
    /// handle cached at a tick, validated at the next tick boundary.
    std::optional<refrain::host::core::provider_handle> abi_handle_before{};
    bool abi_valid_before_commit{false};
    bool abi_stale_after_commit{false};
    std::uint64_t abi_generation_after_commit{0};
    bool abi_missing_after_removal{false};
};

} // namespace

const suite mod_lifecycle_wiring_tests = [] {
    "mod lifecycle wiring/console reload publishes at the tick boundary and "
    "stales the old handle"_test = [] {
        scratch_directory scratch{};
        auto archive{
            pack_mod(scratch, "leaf", manifest_text(leaf_id, "refrain:leaf"))};
        expect(archive.has_value());
        if (!archive) {
            return;
        }
        auto store_root{install_archive(scratch, *archive, "store")};
        expect(store_root.has_value());
        if (!store_root) {
            return;
        }
        const auto config_dir{write_config_dir(scratch, "config", true, true)};
        auto spec{parse_spec(pinned_spec(leaf_id, "leaf"))};
        expect(static_cast<bool>(spec));
        if (!spec) {
            return;
        }
        const std::vector<std::string> roots{*store_root};
        wired_host fixture{};
        if (!load_host(fixture, *spec, roots)) {
            return;
        }
        expect(fixture.host.epoch() == 1U);
        expect(fixture.host.mods().size() == 1U);
        const auto digest_before{fixture.host.composition_digest()};
        const auto initial_query{fixture.host.services().query("refrain:leaf")};
        expect(initial_query.has_value());
        if (!initial_query) {
            return;
        }
        expect(initial_query->generation == 1U);
        const std::optional<kernel::provider_handle> initial_handle{
            *initial_query};

        const auto world_path{scratch.path() / "world"};
        expect(create_flat_world(world_path)) << "flat world creation failed";
        const auto script{scratch.path() / "commands.txt"};
        expect(write_text(script, "0 mods reload " + std::string{leaf_id} +
                                      "\n2 mods uninstall " +
                                      std::string{leaf_id} + " confirm\n"))
            << "script write failed";

        server::server_run_options options{};
        options.world_path = world_path.string();
        options.ticks = 4;
        options.script_file = script.string();
        options.config_dir = config_dir;
        options.content_store = *store_root;
        options.mod_host = &fixture.host;
        observations seen{};
        options.on_tick = [&fixture, &seen](std::uint64_t tick) {
            const auto host_core{
                refrain::host::make_core_provider(fixture.context)};
            if (tick == 1) {
                seen.epoch_before_commit = fixture.host.epoch();
                seen.staged_before_commit =
                    fixture.host.staged_change().has_value();
                if (const auto handle{
                        fixture.host.services().query("refrain:leaf")};
                    handle) {
                    seen.oldest_handle = *handle;
                }
                // A mod caches the generation-stamped handle at the tick and
                // validates it at the next boundary.
                if (const auto handle{
                        host_core.query_service_handle("refrain:leaf", {})};
                    handle) {
                    seen.abi_handle_before = *handle;
                    seen.abi_valid_before_commit =
                        host_core
                            .validate_handle("refrain:leaf", {},
                                             handle->generation)
                            .has_value();
                }
                return;
            }
            if (tick == 2) {
                seen.epoch_after_commit = fixture.host.epoch();
                const auto record{fixture.host.lifecycle_record(leaf_id)};
                expect(record.has_value());
                if (record) {
                    seen.generation_after_commit = record->generation;
                }
                if (const auto handle{
                        fixture.host.services().query("refrain:leaf")};
                    handle) {
                    seen.new_handle = *handle;
                }
                // The generation cached before the commit is stale; the
                // re-query returns the new generation.
                if (seen.abi_handle_before.has_value()) {
                    const auto stale{host_core.validate_handle(
                        "refrain:leaf", {},
                        seen.abi_handle_before->generation)};
                    seen.abi_stale_after_commit =
                        !stale.has_value() &&
                        stale.error() == refrain::host::core::host_error::stale;
                }
                if (const auto handle{
                        host_core.query_service_handle("refrain:leaf", {})};
                    handle) {
                    seen.abi_generation_after_commit = handle->generation;
                }
                return;
            }
            if (tick == 4) {
                seen.epoch_after_removal = fixture.host.epoch();
                seen.composition_without_mod =
                    fixture.host.find(leaf_id) == nullptr;
                const auto gone{
                    host_core.query_service_handle("refrain:leaf", {})};
                seen.abi_missing_after_removal =
                    !gone.has_value() &&
                    gone.error() == refrain::host::core::host_error::not_found;
            }
        };
        server::publish_mod_lifecycle_inputs(server::mod_lifecycle_inputs{
            .store_roots = roots, .auto_scan = true});
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
        // The composition-root handoff is consumed by the run, so a second
        // runtime in the same process cannot silently reuse this catalog.
        expect(!server::take_mod_lifecycle_inputs().has_value());
        expect(seen.epoch_before_commit == 1U);
        expect(seen.staged_before_commit);
        expect(seen.oldest_handle.has_value());
        expect(seen.epoch_after_commit == 2U);
        expect(seen.generation_after_commit == 2U);
        expect(seen.new_handle.has_value());
        if (seen.new_handle) {
            expect(seen.new_handle->generation == 2U);
        }
        expect(seen.epoch_after_removal == 3U);
        expect(seen.composition_without_mod);
        // The host core ABI saw the same composition: a handle queried before
        // the commit validated, the commit staled it, the re-query advanced the
        // generation, and the removal turned the query into not_found.
        expect(seen.abi_handle_before.has_value());
        if (seen.abi_handle_before) {
            expect(seen.abi_handle_before->generation == 1U);
            expect(seen.abi_handle_before->epoch == 1U);
        }
        expect(seen.abi_valid_before_commit);
        expect(seen.abi_stale_after_commit);
        expect(seen.abi_generation_after_commit == 2U);
        expect(seen.abi_missing_after_removal);
        // The running composition followed the console command.
        expect(fixture.host.epoch() == 3U);
        expect(fixture.host.mods().empty());
        expect(fixture.host.composition_digest() != digest_before);
        expect(!fixture.host.staged_change().has_value());
        // The generation the tick committed into the running composition is
        // stale: both the pre-reload and the pre-uninstall handle.
        for (const auto &handle :
             {seen.oldest_handle, seen.new_handle, initial_handle}) {
            if (!handle.has_value()) {
                expect(false) << "expected a captured provider handle";
                continue;
            }
            const auto verdict{fixture.host.services().validate(*handle)};
            expect(!verdict.has_value());
            if (!verdict) {
                expect(verdict.error().code == refrain::error_code::stale);
            }
        }
        const auto receipts{mod::list_receipt_paths(*store_root)};
        expect(receipts.has_value());
        if (receipts) {
            expect(receipts->empty());
        }
        log.stop();
        const auto text{log.text()};
        expect(contains(text, "ran reload on refrain.test.wire.leaf 1.0.0"))
            << text;
        expect(contains(text, "the result was ok")) << text;
        expect(contains(text, "ran uninstall on refrain.test.wire.leaf"))
            << text;
        expect(contains(text, "published composition epoch 2")) << text;
        expect(contains(text, "published composition epoch 3")) << text;
    };

    "mod lifecycle wiring/console install adds a leaf mod to the running "
    "composition"_test = [] {
        scratch_directory scratch{};
        auto archive{
            pack_mod(scratch, "leaf", manifest_text(leaf_id, "refrain:leaf"))};
        expect(archive.has_value());
        if (!archive) {
            return;
        }
        // Create the store before the server starts: an existing store without
        // receipts is an empty catalog.
        const auto store_root_path{scratch.path() / "store"};
        {
            auto store{data::content_store::open(store_root_path.string())};
            expect(static_cast<bool>(store));
            if (!store) {
                return;
            }
        }
        const auto store_root{store_root_path.string()};
        const auto config_dir{write_config_dir(scratch, "config", true, false)};
        auto spec{parse_spec(empty_spec_text)};
        expect(static_cast<bool>(spec));
        if (!spec) {
            return;
        }
        const std::vector<std::string> roots{store_root};
        wired_host fixture{};
        if (!load_host(fixture, *spec, roots)) {
            return;
        }
        expect(fixture.host.epoch() == 1U);
        expect(fixture.host.mods().empty());

        const auto world_path{scratch.path() / "world"};
        expect(create_flat_world(world_path)) << "flat world creation failed";
        const auto script{scratch.path() / "commands.txt"};
        expect(write_text(script, "0 mods install " + *archive + " confirm\n"))
            << "script write failed";
        server::server_run_options options{};
        options.world_path = world_path.string();
        options.ticks = 2;
        options.script_file = script.string();
        options.config_dir = config_dir;
        options.content_store = store_root;
        options.mod_host = &fixture.host;
        std::uint64_t epoch_before_commit{0};
        bool staged_before_commit{false};
        std::uint64_t epoch_after_commit{0};
        options.on_tick = [&fixture, &epoch_before_commit,
                           &staged_before_commit,
                           &epoch_after_commit](std::uint64_t tick) {
            if (tick == 1) {
                epoch_before_commit = fixture.host.epoch();
                staged_before_commit = fixture.host.staged_change().has_value();
            } else if (tick == 2) {
                epoch_after_commit = fixture.host.epoch();
            }
        };
        server::publish_mod_lifecycle_inputs(server::mod_lifecycle_inputs{
            .store_roots = roots, .auto_scan = true});
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
        // The receipt was written by the command, the composition change was
        // staged, and the next tick boundary published it.
        expect(epoch_before_commit == 1U);
        expect(staged_before_commit);
        expect(epoch_after_commit == 2U);
        expect(fixture.host.epoch() == 2U);
        expect(fixture.host.find(leaf_id) != nullptr);
        expect(fixture.host.mods().size() == 1U);
        const auto record{fixture.host.lifecycle_record(leaf_id)};
        expect(record.has_value());
        if (record) {
            expect(record->generation == 1U);
            expect(!record->quarantined);
        }
        const auto receipts{mod::list_receipt_paths(store_root)};
        expect(receipts.has_value());
        if (receipts) {
            expect(receipts->size() == 1U);
        }
        log.stop();
        const auto text{log.text()};
        expect(contains(text, "ran install on refrain.test.wire.leaf 1.0.0"))
            << text;
        expect(contains(text, "the result was ok")) << text;
        expect(contains(text, "published composition epoch 2")) << text;
    };

    "mod lifecycle wiring/gates off keep the running composition and stage "
    "nothing"_test = [] {
        scratch_directory scratch{};
        auto archive{
            pack_mod(scratch, "leaf", manifest_text(leaf_id, "refrain:leaf"))};
        auto extra{pack_mod(
            scratch, "extra",
            manifest_text("refrain.test.wire.extra", "refrain:extra"))};
        expect(archive.has_value());
        expect(extra.has_value());
        if (!archive || !extra) {
            return;
        }
        auto store_root{install_archive(scratch, *archive, "store")};
        expect(store_root.has_value());
        if (!store_root) {
            return;
        }
        const auto config_dir{
            write_config_dir(scratch, "config", false, false)};
        auto spec{parse_spec(pinned_spec(leaf_id, "leaf"))};
        expect(static_cast<bool>(spec));
        if (!spec) {
            return;
        }
        const std::vector<std::string> roots{*store_root};
        wired_host fixture{};
        if (!load_host(fixture, *spec, roots)) {
            return;
        }
        const auto digest_before{fixture.host.composition_digest()};
        const auto epoch_before{fixture.host.epoch()};

        const auto world_path{scratch.path() / "world"};
        expect(create_flat_world(world_path)) << "flat world creation failed";
        const auto script{scratch.path() / "commands.txt"};
        const auto script_text{"0 mods reload " + std::string{leaf_id} +
                               "\n1 mods install " + *extra +
                               " confirm\n2 mods uninstall " +
                               std::string{leaf_id} + " confirm\n"};
        expect(write_text(script, script_text)) << "script write failed";
        server::server_run_options options{};
        options.world_path = world_path.string();
        options.ticks = 3;
        options.script_file = script.string();
        options.config_dir = config_dir;
        options.content_store = *store_root;
        options.mod_host = &fixture.host;
        server::publish_mod_lifecycle_inputs(server::mod_lifecycle_inputs{
            .store_roots = roots, .auto_scan = true});
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
        // Nothing was published: no reload, no install activation and no
        // removal reached the running composition.
        expect(fixture.host.epoch() == epoch_before);
        expect(fixture.host.composition_digest() == digest_before);
        expect(!fixture.host.staged_change().has_value());
        expect(fixture.host.mods().size() == 1U);
        expect(fixture.host.find(leaf_id) != nullptr);
        expect(fixture.host.find("refrain.test.wire.extra") == nullptr);
        // The store-side behavior is unchanged: install and uninstall still
        // change the content store and report that a restart is required.
        const auto receipts{mod::list_receipt_paths(*store_root)};
        expect(receipts.has_value());
        if (receipts) {
            expect(receipts->size() == 1U);
        }
        log.stop();
        const auto text{log.text()};
        expect(contains(text, "hot reload is disabled")) << text;
        expect(contains(text, "restart required")) << text;
        // No tick-boundary commit happened.
        expect(!contains(text, "published composition epoch")) << text;
    };

    "mod lifecycle wiring/restart-required provider refuses reload naming the "
    "service"_test = [] {
        scratch_directory scratch{};
        auto archive{
            pack_mod(scratch, "core", manifest_text(core_id, "refrain:sim"))};
        expect(archive.has_value());
        if (!archive) {
            return;
        }
        auto store_root{install_archive(scratch, *archive, "store")};
        expect(store_root.has_value());
        if (!store_root) {
            return;
        }
        const auto config_dir{write_config_dir(scratch, "config", true, true)};
        auto spec{parse_spec(pinned_spec(core_id, "sim"))};
        expect(static_cast<bool>(spec));
        if (!spec) {
            return;
        }
        const std::vector<std::string> roots{*store_root};
        wired_host fixture{};
        fixture.loader.guest_service = "refrain:sim";
        if (!load_host(fixture, *spec, roots)) {
            return;
        }
        expect(fixture.host.epoch() == 1U);
        expect(fixture.host.mods().size() == 1U);

        const auto world_path{scratch.path() / "world"};
        expect(create_flat_world(world_path)) << "flat world creation failed";
        const auto script{scratch.path() / "commands.txt"};
        expect(
            write_text(script, "0 mods reload " + std::string{core_id} + "\n"))
            << "script write failed";
        server::server_run_options options{};
        options.world_path = world_path.string();
        options.ticks = 1;
        options.script_file = script.string();
        options.config_dir = config_dir;
        options.content_store = *store_root;
        options.mod_host = &fixture.host;
        server::publish_mod_lifecycle_inputs(server::mod_lifecycle_inputs{
            .store_roots = roots, .auto_scan = true});
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
        // The refusal names the service and leaves the composition alone.
        expect(fixture.host.epoch() == 1U);
        expect(!fixture.host.staged_change().has_value());
        expect(fixture.host.find(core_id) != nullptr);
        log.stop();
        const auto text{log.text()};
        expect(contains(text, "refrain:sim")) << text;
        expect(contains(text, "restart-required")) << text;
        expect(!contains(text, "published composition epoch")) << text;
    };

    "mod lifecycle wiring/install names the catalog rule that keeps the "
    "receipt out of the composition"_test = [] {
        // The gate is on, but the gamepack pins the composition and auto-scan
        // is off: the receipt is installed for the next start, and the command
        // names why the running composition cannot include it instead of
        // publishing an empty catalog change.
        scratch_directory scratch{};
        auto archive{
            pack_mod(scratch, "leaf", manifest_text(leaf_id, "refrain:leaf"))};
        auto extra{pack_mod(
            scratch, "extra",
            manifest_text("refrain.test.wire.extra", "refrain:extra"))};
        expect(archive.has_value());
        expect(extra.has_value());
        if (!archive || !extra) {
            return;
        }
        auto store_root{install_archive(scratch, *archive, "store")};
        expect(store_root.has_value());
        if (!store_root) {
            return;
        }
        const auto config_dir{write_config_dir(scratch, "config", true, false)};
        auto spec{parse_spec(pinned_spec(leaf_id, "leaf", false))};
        expect(static_cast<bool>(spec));
        if (!spec) {
            return;
        }
        const std::vector<std::string> roots{*store_root};
        wired_host fixture{};
        if (!load_host(fixture, *spec, roots)) {
            return;
        }
        expect(fixture.host.epoch() == 1U);
        const auto world_path{scratch.path() / "world"};
        expect(create_flat_world(world_path)) << "flat world creation failed";
        const auto script{scratch.path() / "commands.txt"};
        expect(write_text(script, "0 mods install " + *extra + " confirm\n"))
            << "script write failed";
        server::server_run_options options{};
        options.world_path = world_path.string();
        options.ticks = 2;
        options.script_file = script.string();
        options.config_dir = config_dir;
        options.content_store = *store_root;
        options.mod_host = &fixture.host;
        server::publish_mod_lifecycle_inputs(server::mod_lifecycle_inputs{
            .store_roots = roots, .auto_scan = false});
        log_capture log{scratch, "console"};
        direct_handles handles{};
        auto port{server::make_world_port(handles.handles)};
        expect(port != nullptr);
        if (!port) {
            return;
        }
        expect(server::run_server(options, handles.handles, *port) == 0);
        // Nothing was staged or published; the receipt waits for a start
        // whose catalog rules select it.
        expect(fixture.host.epoch() == 1U);
        expect(!fixture.host.staged_change().has_value());
        expect(fixture.host.find("refrain.test.wire.extra") == nullptr);
        const auto receipts{mod::list_receipt_paths(*store_root)};
        expect(receipts.has_value());
        if (receipts) {
            expect(receipts->size() == 2U);
        }
        log.stop();
        const auto text{log.text()};
        expect(contains(text, "not part of the resolved composition")) << text;
        expect(contains(text, "auto-scan is off")) << text;
        expect(!contains(text, "published composition epoch")) << text;
    };

    "mod lifecycle wiring/the content store wires the lifecycle when nothing "
    "was published"_test = [] {
        // An embedder (or a test) that builds the run options directly does
        // not go through the composition-root handoff. The content store is
        // then the whole catalog and the gamepack's own auto-scan flag is
        // kept.
        scratch_directory scratch{};
        auto archive{
            pack_mod(scratch, "leaf", manifest_text(leaf_id, "refrain:leaf"))};
        expect(archive.has_value());
        if (!archive) {
            return;
        }
        auto store_root{install_archive(scratch, *archive, "store")};
        expect(store_root.has_value());
        if (!store_root) {
            return;
        }
        const auto config_dir{write_config_dir(scratch, "config", false, true)};
        auto spec{parse_spec(pinned_spec(leaf_id, "leaf"))};
        expect(static_cast<bool>(spec));
        if (!spec) {
            return;
        }
        const std::vector<std::string> roots{*store_root};
        wired_host fixture{};
        if (!load_host(fixture, *spec, roots)) {
            return;
        }
        expect(fixture.host.epoch() == 1U);
        const auto world_path{scratch.path() / "world"};
        expect(create_flat_world(world_path)) << "flat world creation failed";
        const auto script{scratch.path() / "commands.txt"};
        expect(
            write_text(script, "0 mods reload " + std::string{leaf_id} + "\n"))
            << "script write failed";
        server::server_run_options options{};
        options.world_path = world_path.string();
        options.ticks = 2;
        options.script_file = script.string();
        options.config_dir = config_dir;
        options.content_store = *store_root;
        options.mod_host = &fixture.host;
        log_capture log{scratch, "console"};
        direct_handles handles{};
        auto port{server::make_world_port(handles.handles)};
        expect(port != nullptr);
        if (!port) {
            return;
        }
        expect(server::run_server(options, handles.handles, *port) == 0);
        expect(fixture.host.epoch() == 2U);
        log.stop();
        const auto text{log.text()};
        expect(contains(text, "the hot lifecycle is wired to 1 catalog root"))
            << text;
        expect(contains(text, "published composition epoch 2")) << text;
    };
};
