// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/app/composition.hpp>

#include <utility>

namespace refrain::app {

auto resolve_server_mods(const mod::gamepack_spec &spec,
                         const server_mods_options &options)
    -> result<mod::composition> {
    return mod::resolve_composition(
        spec, options.roots,
        mod::composition_load_options{.builtin_image = options.builtin_image,
                                      .required_services =
                                          options.required_services,
                                      .discovery = options.discovery,
                                      .auto_scan = options.auto_scan});
}

auto activate_server_mods(mod::host &host, const mod::composition &composition,
                          std::span<const std::byte> image) -> status {
    return host.load(composition, image);
}

auto load_server_mods(const mod::gamepack_spec &spec,
                      const server_mods_options &options)
    -> result<std::unique_ptr<server_mods>> {
    auto loaded{std::make_unique<server_mods>()};
    auto composition{resolve_server_mods(spec, options)};
    if (!composition) {
        return std::unexpected{std::move(composition.error())};
    }
    loaded->composition = std::move(*composition);
    if (auto activated{activate_server_mods(loaded->host, loaded->composition,
                                            options.builtin_image)};
        !activated) {
        return std::unexpected{std::move(activated.error())};
    }
    return loaded;
}

} // namespace refrain::app
