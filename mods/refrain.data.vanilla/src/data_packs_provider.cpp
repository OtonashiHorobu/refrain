// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
//
// `refrain:data.packs` provider implementation for the bundled mod
// `refrain.data.vanilla`. The mod is statically linked into the host and
// registered through REFR_MOD_STATIC; the factory publishes the generated
// C ABI vtable so the engine can mount packs and read resources through the
// contract instead of touching data::vfs directly.

#include <refrain/contracts/abi.h>
#include <refrain/contracts/abi.hpp>
#include <refrain/contracts/services/data_packs_abi.h>
#include <refrain/core/error.hpp>
#include <refrain/core/io.hpp>
#include <refrain/data/builtin_image.hpp>
#include <refrain/data/vfs.hpp>
#include <refrain/kernel/static_mod.hpp>
#include <refrain/mod/host.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace contracts = refrain::contracts;
namespace data = refrain::data;
namespace kernel = refrain::kernel;

constexpr std::uint32_t pack_not_found{
    REFRAIN_SERVICES_DATA_PACKS_PACK_ERROR_NOT_FOUND};
constexpr std::uint32_t pack_invalid_format{
    REFRAIN_SERVICES_DATA_PACKS_PACK_ERROR_INVALID_FORMAT};
constexpr std::uint32_t pack_io_error{
    REFRAIN_SERVICES_DATA_PACKS_PACK_ERROR_IO_ERROR};
constexpr std::uint32_t pack_already_mounted{
    REFRAIN_SERVICES_DATA_PACKS_PACK_ERROR_ALREADY_MOUNTED};

[[nodiscard]] auto status_of(const refrain::error &error) -> std::uint32_t {
    switch (error.code) {
    case refrain::error_code::not_found:
        return pack_not_found;
    case refrain::error_code::io_error:
        return pack_io_error;
    default:
        return pack_invalid_format;
    }
}

class provider_impl {
  public:
    explicit provider_impl(std::span<const std::byte> builtin_image) {
        // Own the builtin image bytes: the pack borrows them (RFSI v1), so
        // the storage must outlive the provider.
        builtin_image_.assign(builtin_image.begin(), builtin_image.end());
        if (!builtin_image_.empty()) {
            auto pack{
                data::open_builtin_image_pack(builtin_image_, "builtin.image")};
            if (pack) {
                static_cast<void>(mount_pack(std::move(*pack),
                                             "refrain:builtin", "1.0.0", 0));
            }
        }
    }

    provider_impl(const provider_impl &) = delete;
    auto operator=(const provider_impl &) -> provider_impl & = delete;
    provider_impl(provider_impl &&) = delete;
    auto operator=(provider_impl &&) -> provider_impl & = delete;
    ~provider_impl() = default;

    static auto mount(void *self,
                      const refrain_services_data_packs_pack_source *source,
                      refrain_services_data_packs_pack_info *out_value)
        -> std::uint32_t {
        auto *provider{static_cast<provider_impl *>(self)};
        if (provider == nullptr || source == nullptr || out_value == nullptr) {
            return pack_invalid_format;
        }
        return provider->do_mount(*source, *out_value);
    }

    static auto unmount(void *self,
                        refrain_services_data_packs_handle_pack handle)
        -> std::uint32_t {
        auto *provider{static_cast<provider_impl *>(self)};
        if (provider == nullptr) {
            return pack_invalid_format;
        }
        const auto found{provider->packs_.find(handle)};
        if (found == provider->packs_.end()) {
            return pack_not_found;
        }
        provider->packs_.erase(found);
        return REFRAIN_STATUS_OK;
    }

    static auto mounted_count(void *self) -> std::uint32_t {
        const auto *provider{static_cast<const provider_impl *>(self)};
        return provider == nullptr
                   ? 0U
                   : static_cast<std::uint32_t>(provider->packs_.size());
    }

    static auto read_resource(void *self,
                              refrain_services_data_packs_handle_pack handle,
                              refrain_str name, refrain_bytes *out_value)
        -> std::uint32_t {
        auto *provider{static_cast<provider_impl *>(self)};
        if (provider == nullptr || out_value == nullptr) {
            return pack_invalid_format;
        }
        const auto found{provider->packs_.find(handle)};
        if (found == provider->packs_.end()) {
            return pack_not_found;
        }
        auto bytes{found->second.pack->read(contracts::as_string_view(name))};
        if (!bytes) {
            return status_of(bytes.error());
        }
        // Contract: borrowed bytes stay valid until the next registry mutation.
        // Keeping one cache buffer per mount gives exactly that lifetime.
        found->second.cache = std::move(*bytes);
        *out_value = refrain_bytes{
            reinterpret_cast<const std::uint8_t *>(found->second.cache.data()),
            static_cast<std::uint64_t>(found->second.cache.size())};
        return REFRAIN_STATUS_OK;
    }

  private:
    struct mounted_pack {
        std::unique_ptr<data::pack> pack{};
        std::string id{};
        std::string version{};
        std::uint32_t pack_format{0};
        std::vector<std::byte> cache{};
    };

    [[nodiscard]] auto mount_pack(std::unique_ptr<data::pack> pack,
                                  std::string id, std::string version,
                                  std::uint32_t pack_format)
        -> std::pair<refrain_services_data_packs_handle_pack, mounted_pack *> {
        const auto handle{next_handle_++};
        auto entry{mounted_pack{.pack = std::move(pack),
                                .id = std::move(id),
                                .version = std::move(version),
                                .pack_format = pack_format}};
        const auto [position, inserted] =
            packs_.emplace(handle, std::move(entry));
        static_cast<void>(inserted);
        return {handle, &position->second};
    }

    [[nodiscard]] auto
    do_mount(const refrain_services_data_packs_pack_source &source,
             refrain_services_data_packs_pack_info &out_value)
        -> std::uint32_t {
        const auto path{contracts::as_string_view(source.path)};
        if (path.empty()) {
            return pack_invalid_format;
        }
        for (const auto &[handle, entry] : packs_) {
            static_cast<void>(handle);
            if (entry.id == path) {
                return pack_already_mounted;
            }
        }

        std::unique_ptr<data::pack> pack{};
        if (refrain::core::io::is_directory(path)) {
            auto opened{data::open_dir_pack(path, std::string{path})};
            if (!opened) {
                return status_of(opened.error());
            }
            pack = std::move(*opened);
        } else {
            // A file source is a zip/jar or a datapack archive; the factory
            // validates the container itself, so a random file fails as
            // invalid_format.
            auto zip{data::open_zip_pack(path, std::string{path})};
            if (!zip) {
                return status_of(zip.error());
            }
            pack = std::move(*zip);
        }
        if (!pack) {
            return pack_invalid_format;
        }

        auto [handle, entry] = mount_pack(std::move(pack), std::string{path},
                                          "1.0.0", source.pack_format);
        out_value = refrain_services_data_packs_pack_info{};
        contracts::init_struct(out_value);
        out_value.pack = handle;
        out_value.id = contracts::to_borrowed(entry->id);
        out_value.version = contracts::to_borrowed(entry->version);
        out_value.pack_format = entry->pack_format;
        out_value.file_count = entry->pack->file_count();
        return REFRAIN_STATUS_OK;
    }

    std::vector<std::byte> builtin_image_{};
    std::map<refrain_services_data_packs_handle_pack, mounted_pack> packs_{};
    refrain_services_data_packs_handle_pack next_handle_{1};
};

auto data_packs_vtable() -> const refrain_services_data_packs_vtable & {
    static const refrain_services_data_packs_vtable table{
        .struct_size = REFRAIN_SERVICES_DATA_PACKS_VTABLE_SIZE,
        .abi_major = REFRAIN_SERVICES_DATA_PACKS_ABI_MAJOR,
        .abi_minor = REFRAIN_SERVICES_DATA_PACKS_ABI_MINOR,
        .abi_patch = REFRAIN_SERVICES_DATA_PACKS_ABI_PATCH,
        .mount = &provider_impl::mount,
        .unmount = &provider_impl::unmount,
        .mounted_count = &provider_impl::mounted_count,
        .read_resource = &provider_impl::read_resource,
    };
    return table;
}

auto make_data_vanilla(refrain::mod::host &host) -> refrain::status {
    // The provider is recreated for each host load (tests create several
    // hosts; production loads exactly one). The host copies the vtable/self
    // pair into its own stable storage, so the host's frozen registry never
    // points at the next host's instance.
    static std::unique_ptr<provider_impl> provider{};
    provider = std::make_unique<provider_impl>(host.builtin_image_bytes());
    // `abi_major` arms the host's vtable-header check: the generated table
    // starts with (struct_size, abi_major, abi_minor, abi_patch), so a factory
    // built against an incompatible contract fails at registration.
    const refrain::mod::provider_api api{
        .vtable = &data_packs_vtable(),
        .self = provider.get(),
        .abi_major = REFRAIN_SERVICES_DATA_PACKS_ABI_MAJOR};
    auto status{host.register_provider("refrain:data.packs", api, "1.0.0")};
    if (!status) {
        provider.reset();
        return status;
    }
    return refrain::status_ok();
}

} // namespace

REFR_MOD_STATIC("refrain.data.vanilla", "1.0.0", 1, make_data_vanilla);
