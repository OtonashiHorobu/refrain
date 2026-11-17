// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

// Dynamic module registration is transactional: every module of the list is
// loaded and validated before the first factory reaches the static mod
// registry. A list whose later module is missing must leave the registry
// empty, because the module files that were opened are closed again and a
// registry entry would then point into an unmapped library.

#include <refrain/app/dynamic_modules.hpp>
#include <refrain/mod/static_mod.hpp>

#include <boost/ut.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

using boost::ut::expect;
using namespace boost::ut::literals;
using boost::ut::suite;

namespace app = refrain::app;
namespace mod = refrain::mod;
namespace version = refrain::version;

[[nodiscard]] auto fixture_path() -> std::filesystem::path {
    return std::filesystem::path{REFRAIN_TEST_DYNAMIC_MOD};
}

} // namespace

suite<"app/dynamic-modules"> dynamic_module_tests = [] {
    "a module list whose later file is missing leaves the registry empty"_test =
        [] {
            const auto path{fixture_path()};
            const std::string directory{path.parent_path().string()};
            const std::string present{path.filename().string()};
            const std::string missing{"refrain-test-missing.so"};
            const std::array<version::dynamic_module_record, 2> records{{
                version::dynamic_module_record{
                    .id = "refrain.test.dynamic",
                    .filename = present,
                },
                version::dynamic_module_record{
                    .id = "refrain.test.missing",
                    .filename = missing,
                },
            }};
            mod::static_mod_registry registry{};
            app::loaded_dynamic_modules loaded{};
            const auto registered{app::load_dynamic_module_list(
                registry, records, directory, &app::load_dynamic_module,
                loaded)};
            expect(!registered);
            if (!registered) {
                expect(registered.error().code ==
                       refrain::error_code::mod_error);
                // The diagnostic names the file that could not be opened.
                expect(registered.error().message.find(missing) !=
                       std::string::npos);
            }
            // Nothing from the list is registered and no mapping is left
            // behind: the first module was loaded and then closed again.
            expect(loaded.empty());
            expect(registry.size() == 0U);
            expect(registry.find("refrain.test.dynamic") == nullptr);
        };

    "a duplicate id in the list is rejected before registration"_test = [] {
        const auto path{fixture_path()};
        const std::string directory{path.parent_path().string()};
        const std::string present{path.filename().string()};
        const std::array<version::dynamic_module_record, 2> records{{
            version::dynamic_module_record{
                .id = "refrain.test.dynamic",
                .filename = present,
            },
            version::dynamic_module_record{
                .id = "refrain.test.dynamic",
                .filename = present,
            },
        }};
        mod::static_mod_registry registry{};
        app::loaded_dynamic_modules loaded{};
        const auto registered{app::load_dynamic_module_list(
            registry, records, directory, &app::load_dynamic_module, loaded)};
        expect(!registered);
        if (!registered) {
            expect(registered.error().code ==
                   refrain::error_code::already_exists);
        }
        expect(loaded.empty());
        expect(registry.size() == 0U);
    };

    "a complete module list registers every factory once"_test = [] {
        const auto path{fixture_path()};
        const std::string directory{path.parent_path().string()};
        const std::string present{path.filename().string()};
        const std::array<version::dynamic_module_record, 1> records{{
            version::dynamic_module_record{
                .id = "refrain.test.dynamic",
                .filename = present,
            },
        }};
        mod::static_mod_registry registry{};
        app::loaded_dynamic_modules loaded{};
        const auto registered{app::load_dynamic_module_list(
            registry, records, directory, &app::load_dynamic_module, loaded)};
        expect(static_cast<bool>(registered))
            << (registered ? "" : registered.error().message);
        expect(registry.size() == 1U);
        const auto *entry{registry.find("refrain.test.dynamic")};
        expect(entry != nullptr);
        expect(loaded.size() == 1U);
        if (entry != nullptr && !loaded.empty()) {
            expect(entry->factory == loaded.front()->descriptor.factory);
            expect(entry->version == loaded.front()->descriptor.version);
            expect(entry->abi == loaded.front()->descriptor.abi);
            expect(loaded.front()->library.has_value());
        }
    };
};
