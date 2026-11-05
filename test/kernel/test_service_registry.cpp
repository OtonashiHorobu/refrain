// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/kernel/service_registry.hpp>

#include <boost/ut.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace boost::ut;

[[nodiscard]] auto
make_provider(std::string service, std::string id, std::string version,
              refrain::kernel::capability_bits capabilities = 0)
    -> refrain::kernel::provider_info {
    return refrain::kernel::provider_info{
        .service = std::move(service),
        .id = std::move(id),
        .version = std::move(version),
        .capabilities = capabilities,
        .instance = nullptr,
        .source = "test",
    };
}

[[nodiscard]] auto error_code_of(const refrain::status &value) -> int {
    return value.has_value() ? -1 : static_cast<int>(value.error().code);
}

/// Sorted instance names as a single string, for readable expectations.
[[nodiscard]] auto
instances_of(const refrain::kernel::service_registry &registry,
             std::string_view service) -> std::string {
    std::string joined;
    for (const auto &instance : registry.active_instances(service)) {
        if (!joined.empty()) {
            joined += ",";
        }
        joined += instance;
    }
    return joined;
}

suite<"kernel/service_registry"> service_registry_tests = [] {
    "registry/add and query a provider"_test = [] {
        refrain::kernel::service_registry registry;
        expect(registry.provider_count() == 0U);
        expect(!registry.has_service("refrain:worldgen"));

        const auto added = registry.add_provider(make_provider(
            "refrain:worldgen", "flat@1", "1.0.0", std::uint64_t{0x1F}));
        expect(added.has_value());
        expect(registry.provider_count() == 1U);
        expect(registry.has_service("refrain:worldgen"));
        expect(!registry.has_service("refrain:render"));

        const auto services = registry.services();
        expect(services.size() == 1U);
        if (services.size() == 1U) {
            expect(services[0] == "refrain:worldgen");
        }

        const auto found = registry.find("refrain:worldgen", "flat@1");
        expect(found.has_value());
        if (found) {
            expect(found->version == "1.0.0");
            expect(found->capabilities == std::uint64_t{0x1F});
        }
        expect(!registry.find("refrain:worldgen", "unknown@1").has_value());
        expect(registry.providers("refrain:worldgen").size() == 1U);
        expect(registry.providers("refrain:render").empty());
        expect(!registry.is_frozen());
        expect(registry.active_count() == 0U);
    };

    "registry/duplicate provider is rejected with already_exists"_test = [] {
        refrain::kernel::service_registry registry;
        expect(registry
                   .add_provider(
                       make_provider("refrain:worldgen", "flat@1", "1.0.0"))
                   .has_value());
        expect(registry
                   .add_provider(
                       make_provider("refrain:worldgen", "flat@1", "1.0.0"))
                   .has_value() == false);

        const auto duplicate = registry.add_provider(
            make_provider("refrain:worldgen", "flat@1", "1.0.0"));
        expect(error_code_of(duplicate) ==
               static_cast<int>(refrain::error_code::already_exists));
        expect(registry.provider_count() == 1U);

        // Different version or id are distinct candidates.
        expect(registry
                   .add_provider(
                       make_provider("refrain:worldgen", "flat@1", "2.0.0"))
                   .has_value());
        expect(registry
                   .add_provider(
                       make_provider("refrain:worldgen", "noise@1", "1.0.0"))
                   .has_value());
        expect(registry.provider_count() == 3U);
    };

    "registry/select_active requires a registered provider"_test = [] {
        refrain::kernel::service_registry registry;
        const auto missing =
            registry.select_active("refrain:worldgen", "flat@1");
        expect(error_code_of(missing) ==
               static_cast<int>(refrain::error_code::not_found));

        expect(registry
                   .add_provider(
                       make_provider("refrain:worldgen", "flat@1", "1.0.0"))
                   .has_value());
        expect(
            registry.select_active("refrain:worldgen", "flat@1").has_value());

        const auto active = registry.active_provider("refrain:worldgen");
        expect(active.has_value());
        if (active) {
            expect(active->id == "flat@1");
        }
        expect(!registry.active_provider("refrain:render").has_value());

        const auto active_services = registry.active_services();
        expect(active_services.size() == 1U);
        if (active_services.size() == 1U) {
            expect(active_services[0] == "refrain:worldgen");
        }

        // A provider registered for a different service cannot be activated.
        expect(registry
                   .add_provider(
                       make_provider("refrain:render", "vulkan@1", "1.0.0"))
                   .has_value());
        expect(
            error_code_of(registry.select_active("refrain:render", "flat@1")) ==
            static_cast<int>(refrain::error_code::not_found));
    };

    "registry/selecting a second provider updates the active one"_test = [] {
        refrain::kernel::service_registry registry;
        expect(registry
                   .add_provider(
                       make_provider("refrain:worldgen", "flat@1", "1.0.0"))
                   .has_value());
        expect(registry
                   .add_provider(
                       make_provider("refrain:worldgen", "noise@1", "1.0.0"))
                   .has_value());

        expect(
            registry.select_active("refrain:worldgen", "flat@1").has_value());
        expect(registry.active_provider("refrain:worldgen")->id == "flat@1");

        expect(
            registry.select_active("refrain:worldgen", "noise@1").has_value());
        const auto active = registry.active_provider("refrain:worldgen");
        expect(active.has_value());
        if (active) {
            expect(active->id == "noise@1");
        }
        expect(registry.active_services().size() == 1U);
        // Re-selection updates the default instance, it does not add one.
        expect(registry.active_count() == 1U);
        expect(instances_of(registry, "refrain:worldgen") == "default");
    };

    "registry/dump is sorted, deterministic and marks the active provider"_test =
        [] {
            refrain::kernel::service_registry registry;
            expect(registry.dump() == "(empty)");

            expect(registry
                       .add_provider(make_provider("refrain:render", "vulkan@1",
                                                   "1.0.0", std::uint64_t{0x3}))
                       .has_value());
            expect(
                registry
                    .add_provider(make_provider("refrain:worldgen", "flat@1",
                                                "1.0.0", std::uint64_t{0x1F}))
                    .has_value());
            expect(registry.select_active("refrain:worldgen", "flat@1")
                       .has_value());

            const std::string dump = registry.dump();
            expect(!dump.empty());
            expect(dump.find("refrain:render -> vulkan@1 [1.0.0] "
                             "caps=0x0000000000000003") != std::string::npos);
            expect(dump.find("refrain:worldgen -> flat@1 [1.0.0] "
                             "caps=0x000000000000001f "
                             "(active: default)") != std::string::npos);
            expect(dump == registry.dump());

            // Providers are ordered by service name: render before worldgen.
            const auto render_position = dump.find("refrain:render");
            const auto worldgen_position = dump.find("refrain:worldgen");
            expect(render_position != std::string::npos);
            expect(worldgen_position != std::string::npos);
            expect(render_position < worldgen_position);
        };

    "registry/clear resets providers and active services"_test = [] {
        refrain::kernel::service_registry registry;
        expect(registry
                   .add_provider(
                       make_provider("refrain:worldgen", "flat@1", "1.0.0"))
                   .has_value());
        expect(
            registry.select_active("refrain:worldgen", "flat@1").has_value());
        expect(registry.provider_count() == 1U);

        expect(registry.clear().has_value());

        expect(registry.provider_count() == 0U);
        expect(registry.services().empty());
        expect(registry.active_services().empty());
        expect(registry.active_instances("refrain:worldgen").empty());
        expect(!registry.active_provider("refrain:worldgen").has_value());
        expect(registry.dump() == "(empty)");
        expect(!registry.is_frozen());
    };

    // Multiple versions of the same provider id coexist; the optional version
    // argument selects a specific one and the active provider reports the
    // selected version.
    "registry/select_active honours the version argument"_test = [] {
        refrain::kernel::service_registry registry;
        expect(registry
                   .add_provider(
                       make_provider("refrain:worldgen", "flat@1", "1.0.0"))
                   .has_value());
        expect(registry
                   .add_provider(
                       make_provider("refrain:worldgen", "flat@1", "2.0.0"))
                   .has_value());

        // Unversioned selection keeps the documented "first registered wins".
        expect(
            registry.select_active("refrain:worldgen", "flat@1").has_value());
        expect(registry.active_provider("refrain:worldgen")->version ==
               "1.0.0");

        expect(registry.select_active("refrain:worldgen", "flat@1", "2.0.0")
                   .has_value());
        const auto active = registry.active_provider("refrain:worldgen");
        expect(active.has_value());
        if (active) {
            expect(active->version == "2.0.0");
        }
        expect(
            registry.find("refrain:worldgen", "flat@1", "2.0.0").has_value());
        expect(
            !registry.find("refrain:worldgen", "flat@1", "3.0.0").has_value());
        // Version 1.0.0 is still a candidate, just not the active one.
        expect(registry.providers("refrain:worldgen").size() == 2U);

        // Selecting an unknown version is not_found, not a silent fallback.
        const auto missing =
            registry.select_active("refrain:worldgen", "flat@1", "9.9.9");
        expect(!missing.has_value());
        if (!missing) {
            expect(missing.error().code == refrain::error_code::not_found);
        }
    };

    // Composition mode "instanced": several providers of one service are active
    // at the same time, each addressed by its instance name.
    "registry/instanced composition coexistence"_test = [] {
        refrain::kernel::service_registry registry;
        expect(registry
                   .add_provider(make_provider("refrain:worldgen", "flat@1",
                                               "1.0.0", std::uint64_t{0x1F}))
                   .has_value());
        expect(registry
                   .add_provider(make_provider("refrain:worldgen", "noise@1",
                                               "1.0.0", std::uint64_t{0x2}))
                   .has_value());

        expect(
            registry.select_active("refrain:worldgen", "flat@1", {}, "primary")
                .has_value());
        expect(
            registry
                .select_active("refrain:worldgen", "noise@1", {}, "secondary")
                .has_value());
        // A provider may be active under several instances at once.
        expect(registry.select_active("refrain:worldgen", "flat@1", {}, "debug")
                   .has_value());

        expect(registry.active_count() == 3U);
        expect(instances_of(registry, "refrain:worldgen") ==
               "debug,primary,secondary");

        const auto primary =
            registry.active_provider("refrain:worldgen", "primary");
        expect(primary.has_value());
        if (primary) {
            expect(primary->id == "flat@1");
        }
        const auto secondary =
            registry.active_provider("refrain:worldgen", "secondary");
        expect(secondary.has_value());
        if (secondary) {
            expect(secondary->id == "noise@1");
        }
        const auto debug =
            registry.active_provider("refrain:worldgen", "debug");
        expect(debug.has_value());
        if (debug) {
            expect(debug->id == "flat@1");
        }

        // The explicit default-instance query sees nothing: no default
        // active.
        expect(!registry.active_provider("refrain:worldgen").has_value());
        expect(!registry.active_provider("refrain:worldgen", "default")
                    .has_value());

        const auto active_services = registry.active_services();
        expect(active_services.size() == 1U);
        if (active_services.size() == 1U) {
            expect(active_services[0] == "refrain:worldgen");
        }
    };

    // "exclusive" composition: the upper resolver activates exactly one
    // provider and addresses the default instance.
    "registry/default instance is the empty instance name"_test = [] {
        refrain::kernel::service_registry registry;
        expect(registry
                   .add_provider(
                       make_provider("refrain:worldgen", "flat@1", "1.0.0"))
                   .has_value());
        expect(registry
                   .add_provider(
                       make_provider("refrain:worldgen", "noise@1", "1.0.0"))
                   .has_value());

        expect(
            registry.select_active("refrain:worldgen", "flat@1").has_value());
        expect(instances_of(registry, "refrain:worldgen") == "default");
        expect(registry.active_provider("refrain:worldgen", "default")->id ==
               "flat@1");

        // Explicit "default" and the empty instance are the same slot:
        // selecting replaces the active provider instead of adding an instance.
        expect(
            registry.select_active("refrain:worldgen", "noise@1", {}, "default")
                .has_value());
        expect(registry.active_count() == 1U);
        expect(registry.active_provider("refrain:worldgen")->id == "noise@1");
    };

    "registry/unknown instance has no active provider"_test = [] {
        refrain::kernel::service_registry registry;
        expect(registry
                   .add_provider(
                       make_provider("refrain:worldgen", "flat@1", "1.0.0"))
                   .has_value());
        expect(
            registry.select_active("refrain:worldgen", "flat@1").has_value());

        expect(
            !registry.active_provider("refrain:worldgen", "typo").has_value());
        expect(!registry.active_provider("refrain:render").has_value());
        expect(registry.active_instances("refrain:worldgen").size() == 1U);
        expect(registry.active_instances("refrain:render").empty());
        expect(registry.active_services().size() == 1U);
    };

    "registry/reselecting an instance updates it in place"_test = [] {
        refrain::kernel::service_registry registry;
        expect(registry
                   .add_provider(
                       make_provider("refrain:worldgen", "flat@1", "1.0.0"))
                   .has_value());
        expect(registry
                   .add_provider(
                       make_provider("refrain:worldgen", "noise@1", "1.0.0"))
                   .has_value());

        expect(
            registry.select_active("refrain:worldgen", "flat@1", {}, "primary")
                .has_value());
        expect(
            registry.select_active("refrain:worldgen", "noise@1", {}, "primary")
                .has_value());
        expect(registry.active_count() == 1U);
        expect(registry.active_provider("refrain:worldgen", "primary")->id ==
               "noise@1");
    };

    "registry/version selection is per instance"_test = [] {
        refrain::kernel::service_registry registry;
        expect(registry
                   .add_provider(
                       make_provider("refrain:worldgen", "flat@1", "1.0.0"))
                   .has_value());
        expect(registry
                   .add_provider(
                       make_provider("refrain:worldgen", "flat@1", "2.0.0"))
                   .has_value());

        expect(
            registry.select_active("refrain:worldgen", "flat@1").has_value());
        expect(
            registry
                .select_active("refrain:worldgen", "flat@1", "2.0.0", "primary")
                .has_value());
        expect(registry.active_provider("refrain:worldgen")->version ==
               "1.0.0");
        expect(
            registry.active_provider("refrain:worldgen", "primary")->version ==
            "2.0.0");
        expect(registry.active_count() == 2U);

        // Unknown versions are not_found on any instance.
        expect(error_code_of(registry.select_active(
                   "refrain:worldgen", "flat@1", "9.9.9", "secondary")) ==
               static_cast<int>(refrain::error_code::not_found));
        expect(registry.active_count() == 2U);

        // Updating the default instance to the newer version keeps the primary
        // slot untouched.
        expect(registry.select_active("refrain:worldgen", "flat@1", "2.0.0")
                   .has_value());
        expect(registry.active_provider("refrain:worldgen")->version ==
               "2.0.0");
        expect(
            registry.active_provider("refrain:worldgen", "primary")->version ==
            "2.0.0");
    };

    // Freeze is the post-assembly lifecycle guard for the composition root:
    // mutations fail with `unsupported`, queries (including the copies) keep
    // working, and the frozen state is permanent/idempotent.
    "registry/freeze rejects mutations and keeps queries working"_test = [] {
        refrain::kernel::service_registry registry;
        expect(registry
                   .add_provider(
                       make_provider("refrain:worldgen", "flat@1", "1.0.0"))
                   .has_value());
        expect(
            registry.select_active("refrain:worldgen", "flat@1").has_value());
        expect(
            registry.select_active("refrain:worldgen", "flat@1", {}, "primary")
                .has_value());

        registry.freeze();
        expect(registry.is_frozen());
        registry.freeze(); // idempotent
        expect(registry.is_frozen());

        const auto provider_count = registry.provider_count();
        const auto active_count = registry.active_count();
        const std::string dump = registry.dump();

        const auto add = registry.add_provider(
            make_provider("refrain:worldgen", "noise@1", "1.0.0"));
        expect(error_code_of(add) ==
               static_cast<int>(refrain::error_code::unsupported));
        const auto select = registry.select_active("refrain:worldgen", "flat@1",
                                                   {}, "secondary");
        expect(error_code_of(select) ==
               static_cast<int>(refrain::error_code::unsupported));
        const auto clear = registry.clear();
        expect(error_code_of(clear) ==
               static_cast<int>(refrain::error_code::unsupported));

        // Argument validation still reports invalid_argument before the frozen
        // state check.
        expect(error_code_of(registry.select_active("refrain:worldgen", "")) ==
               static_cast<int>(refrain::error_code::invalid_argument));
        expect(error_code_of(registry.add_provider(
                   make_provider("", "orphan@1", "1.0.0"))) ==
               static_cast<int>(refrain::error_code::invalid_argument));

        // Nothing changed and queries still work on the frozen registry.
        expect(registry.provider_count() == provider_count);
        expect(registry.active_count() == active_count);
        expect(registry.dump() == dump);
        expect(registry.active_provider("refrain:worldgen")->id == "flat@1");
        expect(registry.active_provider("refrain:worldgen", "primary")->id ==
               "flat@1");
        expect(registry.find("refrain:worldgen", "flat@1").has_value());
        expect(instances_of(registry, "refrain:worldgen") == "default,primary");
    };

    // Dump stability: candidate order and instance order come from sorted
    // storage, so the text does not depend on registration/selection order.
    "registry/dump lists active instances deterministically"_test = [] {
        refrain::kernel::service_registry first;
        expect(first
                   .add_provider(make_provider("refrain:render", "vulkan@1",
                                               "1.0.0", std::uint64_t{0x3}))
                   .has_value());
        expect(first
                   .add_provider(make_provider("refrain:worldgen", "flat@1",
                                               "1.0.0", std::uint64_t{0x1F}))
                   .has_value());
        expect(first
                   .add_provider(make_provider("refrain:worldgen", "noise@1",
                                               "1.0.0", std::uint64_t{0x2}))
                   .has_value());
        expect(first.select_active("refrain:worldgen", "flat@1", {}, "default")
                   .has_value());
        expect(first.select_active("refrain:worldgen", "noise@1", {}, "primary")
                   .has_value());
        expect(
            first.select_active("refrain:worldgen", "flat@1", {}, "secondary")
                .has_value());

        refrain::kernel::service_registry second;
        expect(second
                   .add_provider(make_provider("refrain:worldgen", "noise@1",
                                               "1.0.0", std::uint64_t{0x2}))
                   .has_value());
        expect(second
                   .add_provider(make_provider("refrain:worldgen", "flat@1",
                                               "1.0.0", std::uint64_t{0x1F}))
                   .has_value());
        expect(second
                   .add_provider(make_provider("refrain:render", "vulkan@1",
                                               "1.0.0", std::uint64_t{0x3}))
                   .has_value());
        expect(
            second.select_active("refrain:worldgen", "flat@1", {}, "secondary")
                .has_value());
        expect(
            second.select_active("refrain:worldgen", "noise@1", {}, "primary")
                .has_value());
        expect(second.select_active("refrain:worldgen", "flat@1", {}, "default")
                   .has_value());

        const std::string dump = first.dump();
        expect(dump == second.dump());
        expect(dump == first.dump());
        expect(dump.find("refrain service registry: 3 provider(s), 3 active "
                         "instance(s)") != std::string::npos);
        expect(dump.find("refrain:render -> vulkan@1 [1.0.0] "
                         "caps=0x0000000000000003\n") != std::string::npos);
        expect(dump.find("refrain:worldgen -> flat@1 [1.0.0] "
                         "caps=0x000000000000001f (active: default, "
                         "secondary)\n") != std::string::npos);
        expect(dump.find("refrain:worldgen -> noise@1 [1.0.0] "
                         "caps=0x0000000000000002 (active: primary)\n") !=
               std::string::npos);
    };

    // Queries return self-contained copies rather than raw pointers into a
    // std::vector that a concurrent add_provider()/clear() could reallocate
    // (UAF). Hammering the registry from several threads must stay consistent
    // (run under TSan to verify the absence of data races). The instanced write
    // path (per-instance select_active) is
    // exercised concurrently with the same queries.
    "registry/concurrent add and query never dangle"_test = [] {
        refrain::kernel::service_registry registry;
        expect(registry
                   .add_provider(
                       make_provider("refrain:worldgen", "flat@1", "1.0.0"))
                   .has_value());
        expect(
            registry.select_active("refrain:worldgen", "flat@1").has_value());

        constexpr int writer_count{3};
        constexpr int per_writer{100};
        std::atomic<int> mismatches{0};
        std::vector<std::thread> writers{};
        writers.reserve(writer_count);
        for (int writer = 0; writer < writer_count; ++writer) {
            writers.emplace_back([&registry, &mismatches, writer] {
                const std::string instance{"writer-" + std::to_string(writer)};
                for (int index = 0; index < per_writer; ++index) {
                    const std::string id{"extra-" + std::to_string(writer) +
                                         "-" + std::to_string(index)};
                    (void)registry.add_provider(
                        make_provider("refrain:render", id, "1.0.0"));
                }
                // Activate this writer's own instance, then query it: only this
                // thread ever writes that slot, so its value must be stable.
                const std::string id{"extra-" + std::to_string(writer) + "-" +
                                     std::to_string(per_writer - 1)};
                if (!registry.select_active("refrain:render", id, {}, instance)
                         .has_value()) {
                    mismatches.fetch_add(1, std::memory_order_relaxed);
                }
                for (int query = 0; query < 25; ++query) {
                    const auto selected =
                        registry.active_provider("refrain:render", instance);
                    if (!selected || selected->id != id) {
                        mismatches.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        for (int query = 0; query < 500; ++query) {
            const auto active = registry.active_provider("refrain:worldgen");
            if (!active || active->id != "flat@1") {
                mismatches.fetch_add(1, std::memory_order_relaxed);
            }
            const auto found = registry.find("refrain:worldgen", "flat@1");
            if (!found || found->version != "1.0.0") {
                mismatches.fetch_add(1, std::memory_order_relaxed);
            }
            (void)registry.active_instances("refrain:worldgen");
            (void)registry.active_instances("refrain:render");
            (void)registry.active_count();
            (void)registry.has_service("refrain:render");
            (void)registry.provider_count();
        }

        for (auto &writer : writers) {
            writer.join();
        }
        expect(mismatches.load(std::memory_order_relaxed) == 0);
        expect(registry.provider_count() ==
               static_cast<std::size_t>(1 + writer_count * per_writer));
        expect(registry.active_provider("refrain:worldgen")->id == "flat@1");
        expect(registry.active_instances("refrain:render").size() ==
               static_cast<std::size_t>(writer_count));
    };
};

} // namespace
