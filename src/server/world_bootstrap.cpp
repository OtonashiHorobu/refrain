// SPDX-License-Identifier: MIT
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

#include <refrain/server/world_bootstrap.hpp>

#include <refrain/core/log.hpp>
#include <refrain/data/content_view.hpp>
#include <refrain/server/content_reload_inputs.hpp>
#include <refrain/world/create.hpp>
#include <refrain/world/proto_bridge.hpp>
#include <refrain/worldgen/chunk_generator.hpp>
#include <refrain/worldgen/dimension_generator.hpp>

#include <fmt/format.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace refrain::server {

namespace {

constexpr refrain::log::logger log_bootstrap{"world-bootstrap"};

/// The content view the runtime will publish as generation 1, taken from the
/// composition-root handoff and put back when the spawn prefill returns.
///
/// The handoff is published before the runtime starts and consumed later by
/// the tick loop, so the spawn prefill takes it for the duration of the call;
/// the next start would mount the same layers the first streamed chunks will
/// use. This keeps the spawn chunks and the streamed chunks on one generation.
class bootstrap_content_handoff {
  public:
    bootstrap_content_handoff() : inputs_{take_content_reload_inputs()} {}

    bootstrap_content_handoff(const bootstrap_content_handoff &) = delete;
    auto operator=(const bootstrap_content_handoff &)
        -> bootstrap_content_handoff & = delete;

    ~bootstrap_content_handoff() {
        if (inputs_) {
            publish_content_reload_inputs(std::move(*inputs_));
        }
    }

    [[nodiscard]] auto initial() const
        -> const std::shared_ptr<const data::content_view> & {
        static const std::shared_ptr<const data::content_view> empty{};
        return inputs_ ? inputs_->initial : empty;
    }

  private:
    std::optional<content_reload_inputs> inputs_{};
};

/// The generation the spawn prefill binds, or null when the boot view changes
/// no document chunk generation reads. Mirrors the first-generator rule of the
/// chunk generation service, so a fresh world is filled from one generation.
[[nodiscard]] auto
spawn_generation(std::shared_ptr<const data::content_view> view,
                 const world_bootstrap_config &config)
    -> std::shared_ptr<const worldgen::generation_data> {
    auto registries{
        std::make_shared<const data::registry_generation>(view->registries())};
    auto built{worldgen::generation_data::build(std::move(view), registries, 1U,
                                                1U, config.data_root,
                                                config.templates_root)};
    if (!built) {
        log_bootstrap.error(
            "failed to bind the spawn area to content view generation 1, so "
            "the spawn area uses the installed content: {}",
            built.error().message);
        return nullptr;
    }
    if (!(*built)->overlays_generation_content() &&
        (*built)->registries().empty()) {
        return nullptr;
    }
    return *built;
}

} // namespace

auto generate_spawn_area(world_port &world,
                         const world_bootstrap_config &config,
                         refrain::world::chunk_pos center)
    -> result<std::size_t> {
    if (config.data_root.empty()) {
        return std::unexpected{
            make_error(error_code::invalid_argument,
                       "spawn generation needs the datapack data root")};
    }
    // Generate the spawn area from the same content generation the chunk
    // service will stream with. The handoff is taken and restored, so the
    // runtime still receives the boot view it expects.
    bootstrap_content_handoff handoff{};
    std::shared_ptr<const worldgen::generation_data> generation{};
    if (handoff.initial() != nullptr) {
        generation = spawn_generation(handoff.initial(), config);
    }
    auto generator{
        worldgen::chunk_generator::create(worldgen::chunk_generator_config{
            .data_root = config.data_root,
            .templates_root = config.templates_root,
            .seed = config.seed,
            .kind = worldgen::dimension_kind::overworld,
            .target = worldgen::generation_target::full,
            .generation = std::move(generation),
        })};
    if (!generator) {
        return std::unexpected{generator.error()};
    }
    const auto radius{config.radius < 0 ? 0 : config.radius};
    const auto side{static_cast<std::int32_t>(radius * 2 + 1)};
    const auto origin{
        refrain::world::chunk_pos{center.x - radius, center.z - radius}};
    // The spawn square is generated through whole tiles, so every written
    // chunk holds the feature writes its neighbours made across its border.
    auto generated{
        (*generator)
            ->generate_area_tiled(worldgen::chunk_pos{origin.x, origin.z}, side,
                                  side, config.workers)};
    if (!generated) {
        return std::unexpected{generated.error()};
    }
    const auto &biomes{(*generator)->biome_names()};
    std::size_t written{0};
    std::size_t index{0};
    for (const auto &proto : *generated) {
        if (proto == nullptr) {
            return std::unexpected{make_error(
                error_code::unknown, "spawn generator returned no chunk")};
        }
        // `generate_area_tiled` returns the requested square in the same
        // x-outer, z-inner order.
        const auto x{origin.x + static_cast<std::int32_t>(index / side)};
        const auto z{origin.z + static_cast<std::int32_t>(index % side)};
        ++index;
        const refrain::world::chunk_pos pos{x, z};
        auto storage{refrain::world::from_proto_chunk(*proto, biomes)};
        if (!storage) {
            return std::unexpected{storage.error()};
        }
        if (auto stored{world.write_chunk(pos, *storage)}; !stored) {
            return std::unexpected{stored.error()};
        }
        ++written;
    }
    if (auto committed{world.commit()}; !committed) {
        return std::unexpected{committed.error()};
    }
    return written;
}

} // namespace refrain::server
