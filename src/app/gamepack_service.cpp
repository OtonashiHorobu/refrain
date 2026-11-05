// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/app/services.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <refrain/archive/manifest.hpp>
#include <refrain/archive/zip.hpp>
#include <refrain/core/config.hpp>
#include <refrain/core/io.hpp>
#include <refrain/core/log.hpp>
#include <refrain/core/semver.hpp>
#include <refrain/core/version.hpp>
#include <refrain/data/builtin_image.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/content_index.hpp>
#include <refrain/data/content_maintenance.hpp>
#include <refrain/data/content_pack.hpp>
#include <refrain/data/content_segment.hpp>
#include <refrain/data/content_store.hpp>
#include <refrain/data/datapack.hpp>
#include <refrain/data/json.hpp>
#include <refrain/data/path.hpp>
#include <refrain/data/registry.hpp>
#include <refrain/data/store_lock.hpp>
#include <refrain/data/vfs.hpp>
#include <refrain/kernel/crash.hpp>
#include <refrain/mod/discovery.hpp>
#include <refrain/mod/gamepack.hpp>
#include <refrain/mod/host.hpp>
#include <refrain/mod/manifest.hpp>
#include <refrain/mod/mod_ref.hpp>
#include <refrain/mod/rfmod.hpp>
#include <refrain/script/wasm.hpp>
#include <schema/schema.hpp>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace refrain::app {

namespace archive = refrain::archive;
namespace config = refrain::core::config;
namespace core = refrain::core;
namespace data = refrain::data;
namespace io = refrain::core::io;
namespace log = refrain::log;
namespace mod = refrain::mod;
namespace schema = refrain::schema;
namespace script = refrain::script;

// --- gamepack --------------------------------------------------------------

auto run_gamepack_dump(const std::string &path, std::string_view side) -> int {
    auto parsed = config::document::parse_file(path);
    if (!parsed) {
        return report_error(parsed.error());
    }
    std::cout << "# origin: " << parsed->origin() << '\n';
    std::cout << parsed->dump() << '\n';

    auto spec = mod::load_gamepack_spec(path, side);
    if (!spec) {
        return report_error(spec.error());
    }
    auto plan = mod::preview_composition(*spec);
    if (!plan) {
        return report_error(plan.error());
    }
    std::cout << "# resolved plan\n" << plan->format();
    return 0;
}

auto run_gamepack_check(const std::string &path, std::string_view side,
                        const std::vector<std::string> &required) -> int {
    std::vector<std::string_view> services{};
    services.reserve(required.size());
    for (const auto &service : required) {
        services.push_back(service);
    }

    auto spec = mod::load_gamepack_spec(path, side);
    if (!spec) {
        return report_error(spec.error());
    }
    auto plan = mod::preview_composition(*spec, services);
    if (!plan) {
        return report_error(plan.error());
    }
    std::cout << plan->format();
    return 0;
}

} // namespace refrain::app
