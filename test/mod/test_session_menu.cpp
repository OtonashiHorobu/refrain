// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// `refrain:net.session` container-menu model.
//
// A real static observer mod subscribes to the generated menu event types
// through the sealed kernel bus, and the test drives the real
// `refrain.session.vanilla` provider: open, slot change, click result and
// close publish the payloads the observer receives.

#include <refrain/app/host_context.hpp>
#include <refrain/contracts/abi.h>
#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/net_session.hpp>
#include <refrain/core/config.hpp>
#include <refrain/host/kernel_api.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/static_mod.hpp>
#include <refrain/platform/process.hpp>

#include <boost/ut.hpp>

#include <fmt/format.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace boost::ut;

namespace config = refrain::core::config;
namespace fs = std::filesystem;
namespace mod = refrain::mod;
namespace session_service = refrain::services::net_session;

[[nodiscard]] auto scratch_directory() -> fs::path {
    const auto stamp{
        std::chrono::steady_clock::now().time_since_epoch().count()};
    auto path{fs::temp_directory_path() /
              fmt::format("refrain-session-menu-{}-{}",
                          refrain::platform::process_id(), stamp)};
    std::error_code error{};
    fs::create_directories(path, error);
    return path;
}

struct observations {
    std::vector<refrain_services_net_session_menu_opened> opened{};
    std::vector<refrain_services_net_session_menu_closed> closed{};
    std::vector<refrain_services_net_session_menu_slot_changed> slots{};
    std::vector<refrain_services_net_session_menu_clicked> clicks{};
    bool subscribed{false};
};

observations g_observed{};

void on_menu_opened(void *, void *payload, std::uint64_t size) {
    if (size != REFRAIN_SERVICES_NET_SESSION_EVENT_MENU_OPENED_SIZE) {
        return;
    }
    g_observed.opened.push_back(
        *static_cast<const refrain_services_net_session_menu_opened *>(
            payload));
}

void on_menu_closed(void *, void *payload, std::uint64_t size) {
    if (size != REFRAIN_SERVICES_NET_SESSION_EVENT_MENU_CLOSED_SIZE) {
        return;
    }
    g_observed.closed.push_back(
        *static_cast<const refrain_services_net_session_menu_closed *>(
            payload));
}

void on_slot_changed(void *, void *payload, std::uint64_t size) {
    if (size != REFRAIN_SERVICES_NET_SESSION_EVENT_MENU_SLOT_CHANGED_SIZE) {
        return;
    }
    g_observed.slots.push_back(
        *static_cast<const refrain_services_net_session_menu_slot_changed *>(
            payload));
}

void on_menu_clicked(void *, void *payload, std::uint64_t size) {
    if (size != REFRAIN_SERVICES_NET_SESSION_EVENT_MENU_CLICKED_SIZE) {
        return;
    }
    g_observed.clicks.push_back(
        *static_cast<const refrain_services_net_session_menu_clicked *>(
            payload));
}

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
    const auto opened{events.event_subscribe(
        REFRAIN_SERVICES_NET_SESSION_EVENT_MENU_OPENED_TYPE, &on_menu_opened,
        nullptr)};
    const auto closed{events.event_subscribe(
        REFRAIN_SERVICES_NET_SESSION_EVENT_MENU_CLOSED_TYPE, &on_menu_closed,
        nullptr)};
    const auto slots{events.event_subscribe(
        REFRAIN_SERVICES_NET_SESSION_EVENT_MENU_SLOT_CHANGED_TYPE,
        &on_slot_changed, nullptr)};
    const auto clicks{events.event_subscribe(
        REFRAIN_SERVICES_NET_SESSION_EVENT_MENU_CLICKED_TYPE, &on_menu_clicked,
        nullptr)};
    if (!opened || !closed || !slots || !clicks) {
        return std::unexpected{
            refrain::make_error(refrain::error_code::mod_error,
                                "test observer could not subscribe")};
    }
    g_observed.subscribed = true;
    return refrain::status_ok();
}

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

[[nodiscard]] auto make_plan() -> refrain::result<mod::composition> {
    constexpr std::string_view text{R"([gamepack]
id = "refrain.test.contracts"
version = "1.0.0"
side = "server"
abi = "1"
auto_scan = false

[mods]
list = ["refrain.session.vanilla@1.0.0", "refrain.test.menu_observer@1.0.0"]

[providers]
net_session = "refrain.session.vanilla@^1.0.0"
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

REFR_MOD_STATIC("refrain.test.menu_observer", "1.0.0", 1, observer_factory);

[[maybe_unused]] const suite session_menu_tests = [] {
    "session/container menu publishes open, slot change, click and close"_test =
        [] {
            g_observed = observations{};
            const auto directory{scratch_directory()};
            const auto session_root{directory / "mods" / "session"};
            const auto observer_root{directory / "mods" / "observer"};
            expect(write_manifest(session_root, "refrain.session.vanilla",
                                  "refrain:net.session"));
            expect(write_manifest(observer_root, "refrain.test.menu_observer",
                                  {}));
            auto plan{make_plan()};
            expect(static_cast<bool>(plan));
            if (!plan) {
                return;
            }
            mod::static_mod_registry registry{};
            for (const auto &descriptor :
                 mod::static_mod_registry::global().all()) {
                if (descriptor.id == "refrain.session.vanilla" ||
                    descriptor.id == "refrain.test.menu_observer") {
                    expect(static_cast<bool>(registry.add(descriptor)));
                }
            }
            mod::host host{registry};
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
                        candidate.id == "refrain.test.menu_observer"
                            ? observer_root.string()
                            : session_root.string());
                }};
            auto loaded{host.load(*plan, resolver)};
            expect(static_cast<bool>(loaded))
                << (loaded ? std::string{"load ok"} : loaded.error().message);
            if (!loaded) {
                return;
            }
            expect(g_observed.subscribed);

            const auto info{
                host.services().active_provider("refrain:net.session")};
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
            const session_service::provider session{
                *static_cast<const refrain_services_net_session_vtable *>(
                    api->vtable),
                api->self};

            auto opened{session.menu_open(42, 0)};
            expect(opened.has_value());
            if (!opened) {
                return;
            }
            expect(g_observed.opened.size() == 1U);
            if (g_observed.opened.size() == 1U) {
                expect(g_observed.opened.front().menu == opened->raw());
                expect(g_observed.opened.front().connection == 42U);
                expect(g_observed.opened.front().container == 0U);
            }
            // A second menu for the same connection is rejected, and an
            // unknown container layout is not modeled.
            expect(!session.menu_open(42, 0).has_value());
            expect(!session.menu_open(43, 1).has_value());
            expect(g_observed.opened.size() == 1U);

            expect(session.menu_set_slot(*opened, 9, "minecraft:stone", 3)
                       .value_or(false));
            expect(g_observed.slots.size() == 1U);
            if (g_observed.slots.size() == 1U) {
                const auto &event{g_observed.slots.front()};
                expect(event.menu == opened->raw());
                expect(event.slot == 9);
                expect(event.count == 3U);
                expect(refrain::contracts::as_string_view(event.item) ==
                       std::string_view{"minecraft:stone"});
            }
            // The same contents do not publish again, and a slot outside the
            // player menu is rejected.
            expect(!session.menu_set_slot(*opened, 9, "minecraft:stone", 3)
                        .value_or(true));
            expect(!session.menu_set_slot(*opened, 0, "minecraft:stone", 1)
                        .has_value());
            expect(g_observed.slots.size() == 1U);

            auto result{session_service::make_menu_click_result()};
            const auto picked{session.menu_click(*opened, 9, 0, result)};
            expect(picked.has_value());
            if (picked) {
                expect(result.accepted == 1U);
                expect(result.slot_count == 0U);
                expect(result.carried_count == 3U);
                expect(refrain::contracts::as_string_view(
                           result.carried_item) == "minecraft:stone");
            }
            expect(g_observed.clicks.size() == 1U);
            if (g_observed.clicks.size() == 1U) {
                const auto &event{g_observed.clicks.front()};
                expect(event.menu == opened->raw());
                expect(event.slot == 9);
                expect(event.button == 0);
                expect(event.accepted == 1U);
                expect(event.carried_count == 3U);
            }

            // Put the carried stack into the hotbar.
            auto placed{session_service::make_menu_click_result()};
            expect(session.menu_click(*opened, 36, 0, placed).has_value());
            expect(placed.accepted == 1U);
            expect(placed.slot_count == 3U);
            expect(placed.carried_count == 0U);
            expect(g_observed.clicks.size() == 2U);

            expect(session.menu_close(*opened).has_value());
            expect(g_observed.closed.size() == 1U);
            if (g_observed.closed.size() == 1U) {
                expect(g_observed.closed.front().menu == opened->raw());
                expect(g_observed.closed.front().connection == 42U);
            }
            expect(!session.menu_close(*opened).has_value());
            expect(g_observed.closed.size() == 1U);
            std::error_code error;
            fs::remove_all(directory, error);
        };
};

} // namespace
