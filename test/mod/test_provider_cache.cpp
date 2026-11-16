// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// `mod::provider_cache`: a tick-loop consumer caches a provider table with the
// registry generation it came from. A staged replacement does not publish
// until the tick-boundary commit, the commit swaps exactly one snapshot, and a
// retired slot clears it instead of leaving the retired table callable.

#include <refrain/kernel/service_registry.hpp>
#include <refrain/mod/provider_cache.hpp>

#include <boost/ut.hpp>

#include <cstdint>
#include <memory>

namespace {

using boost::ut::expect;
using boost::ut::suite;
using namespace boost::ut::literals;

namespace kernel = refrain::kernel;
namespace mod = refrain::mod;

struct fake_table {
    std::uint32_t struct_size{16};
    std::uint32_t abi_major{1};
    std::uint32_t abi_minor{0};
    std::uint32_t abi_patch{0};
};

[[nodiscard]] auto provider_of(fake_table &table, int &self)
    -> mod::provider_api {
    return mod::provider_api{.vtable = &table, .self = &self, .abi_major = 1};
}

[[nodiscard]] auto register_leaf(mod::provider_api &api)
    -> kernel::provider_info {
    kernel::provider_info info{};
    info.service = "refrain:leaf";
    info.id = "leaf";
    info.version = "1.0.0";
    info.instance = &api;
    return info;
}

const suite provider_cache_tests = [] {
    "mod/provider cache republishes only on a committed generation"_test = [] {
        kernel::service_registry registry;
        fake_table first_table{};
        int first_self{1};
        auto first_api{provider_of(first_table, first_self)};
        auto first_info{register_leaf(first_api)};
        expect(registry.add_provider(first_info).has_value());
        expect(registry.select_active("refrain:leaf", "leaf").has_value());
        const auto first_epoch{registry.commit_epoch()};
        expect(first_epoch.has_value());
        expect(*first_epoch == 1U);

        mod::provider_cache cache{registry, "refrain:leaf"};
        const auto first_snapshot{cache.current()};
        expect(first_snapshot != nullptr);
        if (first_snapshot != nullptr) {
            expect(first_snapshot->generation == 1U);
            expect(first_snapshot->vtable == &first_table);
            expect(first_snapshot->self == &first_self);
        }
        // A refresh without a committed change keeps the same snapshot: the
        // ordinary tick does not allocate.
        expect(cache.refresh());
        expect(cache.current() == first_snapshot);

        // A staged replacement is invisible until the tick-boundary commit.
        fake_table second_table{};
        int second_self{2};
        auto second_api{provider_of(second_table, second_self)};
        auto second_info{register_leaf(second_api)};
        auto serial{registry.add_staged_provider(second_info)};
        expect(serial.has_value());
        if (!serial) {
            return;
        }
        expect(registry.stage_active(*serial, "refrain:leaf").has_value());
        expect(cache.refresh());
        expect(cache.current() == first_snapshot);

        // Quiescing refuses new queries but the committed generation is still
        // the one to call; the cache keeps it.
        expect(registry.quiesce("refrain:leaf").has_value());
        expect(cache.refresh());
        expect(cache.current() == first_snapshot);
        expect(registry.resume("refrain:leaf").has_value());

        const auto second_epoch{registry.commit_epoch()};
        expect(second_epoch.has_value());
        expect(cache.refresh());
        const auto second_snapshot{cache.current()};
        expect(second_snapshot != nullptr);
        if (second_snapshot != nullptr) {
            expect(second_snapshot->generation == 2U);
            expect(second_snapshot->vtable == &second_table);
            expect(second_snapshot->self == &second_self);
        }
        // The old snapshot stays readable while a caller holds it.
        expect(first_snapshot != nullptr);
        if (first_snapshot != nullptr) {
            expect(first_snapshot->generation == 1U);
        }

        // A retired slot clears the cache instead of leaving the retired table
        // reachable through the current snapshot.
        expect(registry.stage_retire("refrain:leaf").has_value());
        expect(registry.commit_epoch().has_value());
        expect(!cache.refresh());
        expect(cache.current() == nullptr);
        expect(cache.generation() == 0U);
    };

    "mod/provider cache starts empty for an inactive slot"_test = [] {
        kernel::service_registry registry;
        mod::provider_cache cache{registry, "refrain:missing"};
        expect(cache.current() == nullptr);
        expect(cache.generation() == 0U);
        expect(!cache.refresh());
        expect(cache.service() == "refrain:missing");
        expect(cache.instance().empty());
    };

    "mod/provider cache binds an instanced slot"_test = [] {
        kernel::service_registry registry;
        fake_table table{};
        int self{1};
        auto api{provider_of(table, self)};
        kernel::provider_info info{};
        info.service = "refrain:leaf";
        info.id = "leaf";
        info.version = "1.0.0";
        info.instance = &api;
        expect(registry.add_provider(std::move(info)).has_value());
        expect(registry.select_active("refrain:leaf", "leaf", {}, "primary")
                   .has_value());
        expect(registry.commit_epoch().has_value());

        mod::provider_cache primary{registry, "refrain:leaf", "primary"};
        expect(primary.current() != nullptr);
        if (const auto snapshot{primary.current()}; snapshot != nullptr) {
            expect(snapshot->generation == 1U);
        }
        expect(primary.instance() == "primary");

        mod::provider_cache missing{registry, "refrain:leaf", "secondary"};
        expect(missing.current() == nullptr);
    };
};

} // namespace
