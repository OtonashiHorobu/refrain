// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// `refrain:sim` block and entity events reach a mod through the generated C
// ABI.
//
// The observer below is a real static mod: the host loads it from a synthetic
// composition and its factory subscribes to the `refrain:host.kernel` event
// bus with the generated event type tags. The test then drives the real
// `refrain.sim.vanilla` provider over a real world on disk, so the observed
// payloads are the ones the provider published, not a fake.

#include <refrain/app/host_context.hpp>
#include <refrain/contracts/abi.h>
#include <refrain/contracts/services/sim.hpp>
#include <refrain/contracts/services/storage.hpp>
#include <refrain/core/config.hpp>
#include <refrain/host/kernel_api.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/static_mod.hpp>
#include <refrain/platform/process.hpp>
#include <refrain/world/model.hpp>
#include <refrain/world/store.hpp>

#include <boost/ut.hpp>

#include <fmt/format.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using namespace boost::ut;

namespace config = refrain::core::config;
namespace fs = std::filesystem;
namespace mod = refrain::mod;
namespace sim_service = refrain::services::sim;
namespace storage = refrain::storage;
namespace world = refrain::world;

[[nodiscard]] auto scratch_directory() -> fs::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    auto path{fs::temp_directory_path() /
              fmt::format("refrain-sim-events-{}-{}",
                          refrain::platform::process_id(), stamp)};
    std::error_code error{};
    fs::create_directories(path, error);
    return path;
}

/// One real chunk at (0, 0): the whole y=0..15 section is stone.
[[nodiscard]] auto make_store(const fs::path &directory)
    -> refrain::result<world::world_store> {
    auto store{world::world_store::create(directory.string())};
    if (!store) {
        return std::unexpected{std::move(store.error())};
    }
    world::chunk chunk{};
    chunk.format_version = world::chunk_format_version;
    chunk.status = storage::chunk_status::full;
    chunk.y_pos = 0;
    storage::section_record section{};
    section.y = 0;
    section.kind = storage::section_kind::single_value;
    section.bits_per_entry = 0;
    section.block_count = static_cast<std::uint16_t>(world::section_volume);
    section.biome_kind = storage::section_kind::single_value;
    section.biome_bits_per_entry = 0;
    section.biome_palette.push_back("minecraft:plains");
    section.palette.push_back(storage::palette_entry{
        .key = "minecraft:stone",
        .count = static_cast<std::uint16_t>(world::section_volume),
        .state = 0,
    });
    chunk.sections.push_back(std::move(section));
    if (auto written{store->write_chunk(world::chunk_pos{0, 0}, chunk)};
        !written) {
        return std::unexpected{std::move(written.error())};
    }
    if (auto committed{store->commit()}; !committed) {
        return std::unexpected{std::move(committed.error())};
    }
    return store;
}

/// Payloads the observer received, in publish order.
struct observations {
    std::vector<refrain_services_sim_block_changed> blocks{};
    std::vector<refrain_services_sim_entity_spawned> spawns{};
    std::vector<refrain_services_sim_entity_removed> removals{};
    bool subscribed{false};
};

observations g_observed{};

void on_block_changed(void *, void *payload, std::uint64_t size) {
    if (size != REFRAIN_SERVICES_SIM_EVENT_BLOCK_CHANGED_SIZE) {
        return;
    }
    g_observed.blocks.push_back(
        *static_cast<const refrain_services_sim_block_changed *>(payload));
}

void on_entity_spawned(void *, void *payload, std::uint64_t size) {
    if (size != REFRAIN_SERVICES_SIM_EVENT_ENTITY_SPAWNED_SIZE) {
        return;
    }
    g_observed.spawns.push_back(
        *static_cast<const refrain_services_sim_entity_spawned *>(payload));
}

void on_entity_removed(void *, void *payload, std::uint64_t size) {
    if (size != REFRAIN_SERVICES_SIM_EVENT_ENTITY_REMOVED_SIZE) {
        return;
    }
    g_observed.removals.push_back(
        *static_cast<const refrain_services_sim_entity_removed *>(payload));
}

/// Static test mod: subscribes to the three sim event types through the sealed
/// kernel service during load.
auto observer_factory(refrain::mod::host &host) -> refrain::status {
    const auto kernel{host.services().active_provider("refrain:host.kernel")};
    if (!kernel || kernel->instance == nullptr) {
        return std::unexpected{
            refrain::make_error(refrain::error_code::unsupported,
                                "test observer needs refrain:host.kernel")};
    }
    const auto *api{
        static_cast<const refrain::mod::provider_api *>(kernel->instance)};
    if (api->vtable == nullptr) {
        return std::unexpected{
            refrain::make_error(refrain::error_code::unsupported,
                                "test observer has no kernel vtable")};
    }
    const refrain::host::kernel::provider events{
        *static_cast<const refrain_host_kernel_vtable *>(api->vtable),
        api->self};
    const auto block{
        events.event_subscribe(REFRAIN_SERVICES_SIM_EVENT_BLOCK_CHANGED_TYPE,
                               &on_block_changed, nullptr)};
    const auto spawn{
        events.event_subscribe(REFRAIN_SERVICES_SIM_EVENT_ENTITY_SPAWNED_TYPE,
                               &on_entity_spawned, nullptr)};
    const auto removal{
        events.event_subscribe(REFRAIN_SERVICES_SIM_EVENT_ENTITY_REMOVED_TYPE,
                               &on_entity_removed, nullptr)};
    if (!block || !spawn || !removal) {
        return std::unexpected{
            refrain::make_error(refrain::error_code::mod_error,
                                "test observer could not subscribe")};
    }
    g_observed.subscribed = true;
    return refrain::status_ok();
}

} // namespace

REFR_MOD_STATIC("refrain.test.sim_observer", "1.0.0", 1, observer_factory);

namespace {

[[nodiscard]] auto write_manifest(const fs::path &root, std::string_view id,
                                  std::string_view provides) -> bool {
    std::error_code error;
    fs::create_directories(root, error);
    if (error) {
        return false;
    }
    auto text{std::string{"[mod]\n"}};
    text += "id = \"" + std::string{id} + "\"\n";
    text += "version = \"1.0.0\"\n";
    text += "abi = \"1\"\n";
    text += "side = \"server\"\n";
    if (!provides.empty()) {
        text +=
            "\n[provides]\nservices = [\"" + std::string{provides} + "\"]\n";
    }
    std::ofstream stream{root / "manifest.toml", std::ios::binary};
    if (!stream) {
        return false;
    }
    stream << text;
    return static_cast<bool>(stream);
}

/// Synthetic composition: the real sim provider plus the observer mod.
[[nodiscard]] auto make_plan() -> refrain::result<mod::composition> {
    constexpr std::string_view text{R"([gamepack]
id = "refrain.test.contracts"
version = "1.0.0"
side = "server"
abi = "1"
auto_scan = false

[mods]
list = ["refrain.sim.vanilla@1.0.0", "refrain.test.sim_observer@1.0.0"]

[providers]
sim = "refrain.sim.vanilla@^1.0.0"
)"};
    auto document{
        config::document::parse_text(std::string{text}, "<gamepack>")};
    if (!document) {
        return std::unexpected{document.error()};
    }
    auto spec{mod::parse_gamepack(*document, "server")};
    if (!spec) {
        return std::unexpected{spec.error()};
    }
    return mod::preview_composition(*spec);
}

[[maybe_unused]] const suite sim_event_tests = [] {
    "sim/provider publishes block changes and entity lifecycle to a mod"_test =
        [] {
            g_observed = observations{};
            const auto directory{scratch_directory()};
            {
                auto store{make_store(directory)};
                expect(static_cast<bool>(store))
                    << (store ? std::string{} : store.error().message);
                if (!store) {
                    return;
                }
            }
            const auto sim_root{directory / "mods" / "sim"};
            const auto observer_root{directory / "mods" / "observer"};
            expect(
                write_manifest(sim_root, "refrain.sim.vanilla", "refrain:sim"));
            expect(
                write_manifest(observer_root, "refrain.test.sim_observer", {}));
            auto plan{make_plan()};
            expect(static_cast<bool>(plan));
            if (!plan) {
                return;
            }
            mod::static_mod_registry registry{};
            for (const auto &descriptor :
                 mod::static_mod_registry::global().all()) {
                if (descriptor.id == "refrain.sim.vanilla" ||
                    descriptor.id == "refrain.test.sim_observer") {
                    expect(static_cast<bool>(registry.add(descriptor)));
                }
            }
            mod::host host{registry};
            // The host registers this sealed provider before load; the observer
            // subscribes through it and the provider publishes through it.
            refrain::app::host_context context{host};
            const mod::provider_api kernel_api{
                .vtable = &refrain::host::kernel_vtable(),
                .self = static_cast<refrain::host::kernel_context *>(&context),
                .abi_major = REFRAIN_HOST_KERNEL_ABI_MAJOR,
            };
            expect(static_cast<bool>(host.register_sealed_provider(
                "refrain:host.kernel", kernel_api, "1.0.0")));
            const auto resolver{
                [&](const mod::candidate &candidate)
                    -> refrain::result<std::unique_ptr<mod::package_source>> {
                    return mod::make_directory_package_source(
                        candidate.id == "refrain.test.sim_observer"
                            ? observer_root.string()
                            : sim_root.string());
                }};
            auto loaded{host.load(*plan, resolver)};
            expect(static_cast<bool>(loaded))
                << (loaded ? std::string{"load ok"} : loaded.error().message);
            if (!loaded) {
                return;
            }
            expect(g_observed.subscribed);

            const auto info{host.services().active_provider("refrain:sim")};
            expect(info.has_value());
            if (!info || info->instance == nullptr) {
                return;
            }
            const auto *api{
                static_cast<const mod::provider_api *>(info->instance)};
            expect(api->vtable != nullptr);
            if (api->vtable == nullptr) {
                return;
            }
            const sim_service::provider sim{
                *static_cast<const refrain_services_sim_vtable *>(api->vtable),
                api->self};
            // The handle names the store the runtime opened; this composition
            // has no storage provider, so the test opens the store it hands
            // over.
            auto store{refrain::world::world_store::open(directory.string())};
            expect(store.has_value())
                << (store ? std::string{} : store.error().message);
            if (!store) {
                return;
            }
            auto sim_config{sim_service::make_sim_config()};
            sim_config.storage =
                refrain::services::storage::make_storage_handle();
            sim_config.storage.store = &*store;
            sim_config.seed = 7;
            auto handle{sim.open_world(sim_config)};
            expect(handle.has_value());
            if (!handle) {
                return;
            }

            // A real block change: stone becomes air at y=1.
            auto change{refrain::core::make_block_change()};
            change.pos = refrain::core::make_block_pos();
            change.pos.x = 0;
            change.pos.y = 1;
            change.pos.z = 0;
            change.new_state = 0;
            const std::array<refrain::core::block_change, 1> changes{change};
            const auto applied{sim.set_blocks(*handle, changes)};
            expect(applied.has_value());
            if (applied) {
                expect(*applied == 1U);
            }
            expect(g_observed.blocks.size() == 1U);
            if (g_observed.blocks.size() == 1U) {
                const auto &event{g_observed.blocks.front()};
                expect(event.x == 0);
                expect(event.y == 1);
                expect(event.z == 0);
                expect(event.new_state == 0U);
                expect(event.old_state != event.new_state);
            }

            // A no-op write publishes nothing.
            auto unchanged{refrain::core::make_block_change()};
            unchanged.pos = refrain::core::make_block_pos();
            unchanged.pos.x = 0;
            unchanged.pos.y = 1;
            unchanged.pos.z = 0;
            unchanged.new_state = 0;
            const std::array<refrain::core::block_change, 1> no_change{
                unchanged};
            const auto skipped{sim.set_blocks(*handle, no_change)};
            expect(skipped.has_value());
            if (skipped) {
                expect(*skipped == 0U);
            }
            expect(g_observed.blocks.size() == 1U);

            // A real entity spawn and removal.
            auto spawned{
                sim.spawn_entity(*handle, "minecraft:pig", 0.5, 20.5, 0.5)};
            expect(spawned.has_value());
            if (!spawned) {
                return;
            }
            expect(g_observed.spawns.size() == 1U);
            if (g_observed.spawns.size() == 1U) {
                const auto &event{g_observed.spawns.front()};
                expect(event.id == *spawned);
                expect(refrain::contracts::as_string_view(event.type) ==
                       "minecraft:pig");
                expect(event.x == 0.5);
                expect(event.y == 20.5);
                expect(event.z == 0.5);
            }
            expect(!sim.spawn_entity(*handle, "", 0.0, 0.0, 0.0).has_value());
            expect(g_observed.spawns.size() == 1U);

            expect(sim.remove_entity(*handle, *spawned));
            expect(g_observed.removals.size() == 1U);
            if (g_observed.removals.size() == 1U) {
                expect(g_observed.removals.front().id == *spawned);
            }
            expect(!sim.remove_entity(*handle, *spawned));
            expect(g_observed.removals.size() == 1U);

            expect(sim.close_world(*handle).has_value());
            std::error_code error;
            fs::remove_all(directory, error);
        };
};

} // namespace
