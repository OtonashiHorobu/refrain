// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/builtin.hpp>
#include <refrain/core/config.hpp>
#include <refrain/data/builtin_image.hpp>
#include <refrain/data/checksum.hpp>
#include <refrain/data/vfs.hpp>
#include <refrain/kernel/manifest.hpp>

#include <boost/ut.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace {

using namespace boost::ut;

namespace config = refrain::core::config;
namespace data = refrain::data;
namespace kernel = refrain::kernel;

constexpr std::string_view k_data_mod_id{"refrain.data.vanilla"};
constexpr std::string_view k_data_manifest_path{
    "mods/refrain.data.vanilla/manifest.toml"};

[[nodiscard]] auto builtin_image_bytes() -> std::span<const std::byte> {
    return std::span<const std::byte>{refrain::builtin::image_data(),
                                      refrain::builtin::image_size()};
}

const suite builtin_image_tests = [] {
    "builtin/image: embedded RFSI is valid and non-empty"_test = [] {
        expect(refrain::builtin::image_data() != nullptr);
        expect(refrain::builtin::image_size() > 0U);
        const auto digest{refrain::builtin::image_digest()};
        expect(digest.size() == 64U);

        auto image{data::builtin_image::open(builtin_image_bytes())};
        expect(static_cast<bool>(image)) << "embedded builtin.image must be a "
                                            "valid RFSI v1 image";
        if (!image) {
            return;
        }
        expect(image->file_count() >= 3U);
        expect(data::to_hex(image->digest()) == digest);
        expect(image->find("mods.index.toml") != nullptr);
        expect(image->find(k_data_manifest_path) != nullptr);
        expect(image->find("mods/refrain.content.vanilla/manifest.toml") !=
               nullptr);
    };

    "builtin/vfs: embedded image mounts as a pack"_test = [] {
        auto pack{data::open_builtin_image_pack(builtin_image_bytes(),
                                                "builtin.image")};
        expect(static_cast<bool>(pack));
        if (!pack) {
            return;
        }
        expect((*pack)->format() == data::pack_format::memory);
        data::vfs vfs{};
        expect(static_cast<bool>(vfs.mount(std::move(*pack))));
        vfs.freeze();
        expect(vfs.is_frozen());
        expect(vfs.pack_count() == 1U);
        expect(vfs.exists(k_data_manifest_path));
        expect(vfs.exists("mods.index.toml"));
        expect(!vfs.exists("mods/refrain.does.not.exist/manifest.toml"));

        // All first-party internal mod manifests (ten mods) live in the
        // builtin image under mods/refrain.*.
        const auto prefixed{vfs.list_prefix("mods/refrain.")};
        expect(prefixed.size() == 10U);
        if (prefixed.size() == 10U) {
            expect(prefixed[1].path ==
                   "mods/refrain.content.vanilla/manifest.toml");
            expect(prefixed[2].path == k_data_manifest_path);
        }
    };

    "builtin/manifest: internal mods parse from the image"_test = [] {
        auto pack{data::open_builtin_image_pack(builtin_image_bytes(),
                                                "builtin.image")};
        expect(static_cast<bool>(pack));
        if (!pack) {
            return;
        }
        data::vfs vfs{};
        expect(static_cast<bool>(vfs.mount(std::move(*pack))));
        vfs.freeze();

        auto text{vfs.read_text(k_data_manifest_path)};
        expect(static_cast<bool>(text));
        if (!text) {
            return;
        }
        auto doc{config::document::parse_text(*text, k_data_manifest_path)};
        expect(static_cast<bool>(doc));
        if (!doc) {
            return;
        }
        auto manifest{kernel::parse_mod_manifest(*doc)};
        expect(static_cast<bool>(manifest));
        if (!manifest) {
            return;
        }
        expect(manifest->common.id == k_data_mod_id);
        expect(manifest->common.version == "1.0.0");
        expect(manifest->common.abi == 1U);
        expect(manifest->common.side == kernel::manifest_side::server);
        expect(!manifest->is_external()); // internal/static mod
        expect(manifest->provides.size() == 1U);
        expect(manifest->provides.front() == "refrain:data.packs");
        expect(manifest->required.size() == 1U);
        expect(manifest->required.front().id == "refrain.content.vanilla");

        // The content mod is internal too and has no [entry].
        auto content_text{
            vfs.read_text("mods/refrain.content.vanilla/manifest.toml")};
        expect(static_cast<bool>(content_text));
        if (content_text) {
            auto content_doc{config::document::parse_text(
                *content_text, "mods/refrain.content.vanilla/manifest.toml")};
            expect(static_cast<bool>(content_doc));
            if (content_doc) {
                auto content{kernel::parse_mod_manifest(*content_doc)};
                expect(static_cast<bool>(content));
                if (content) {
                    expect(!content->is_external());
                    expect(content->common.side == kernel::manifest_side::both);
                }
            }
        }
    };
};

} // namespace
